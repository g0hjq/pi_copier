#include "globals.h"
#include "utilities.h"
#include "usb.h"
#include "lcd.h"

/*
 * USB monitor
 * -----------
 * A background thread polls /sys/block every 200ms for removable sd* devices.
 * Inserts and removals are placed in two small FIFO event queues, guarded by
 * a mutex, so the main thread can drain them without racing or losing events.
 *
 * TODO: replace the polling with a libudev netlink monitor for lower latency
 * and lower CPU. libudev is already included via globals.h.
 */

#define USB_EVENT_QUEUE_LEN (MAX_USB_CHANNELS * 2)
#define USB_POLL_INTERVAL_US 200000

typedef struct {
    NamePathStruct events[USB_EVENT_QUEUE_LEN];
    int head;   // index of next slot to read
    int tail;   // index of next slot to write
    int count;  // number of queued events
} UsbEventQueue;

static pthread_t        usb_monitor_thread;
static pthread_mutex_t  usb_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool    usb_monitor_stop = false;

static UsbEventQueue    inserted_queue;
static UsbEventQueue    removed_queue;

static SharedDataStruct* shared_data_p;
static NamePathStruct    usb_devices_loaded[MAX_USB_CHANNELS];


// Bounded string copy that always null-terminates and is opaque to GCC's
// format-truncation analysis. `dst_size` must be > 0.
static void safe_copy(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t i = 0;
    size_t limit = dst_size - 1;
    while (i < limit && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// Like safe_copy but prepends a fixed prefix.
static void safe_copy_prefixed(char* dst, size_t dst_size,
                               const char* prefix, const char* src)
{
    if (!dst || dst_size == 0) return;

    size_t i = 0;
    size_t limit = dst_size - 1;
    if (prefix) {
        while (i < limit && prefix[i] != '\0') {
            dst[i] = prefix[i];
            i++;
        }
    }
    size_t j = 0;
    if (src) {
        while (i < limit && src[j] != '\0') {
            dst[i++] = src[j++];
        }
    }
    dst[i] = '\0';
}


//------------------------------------------------------------------------------------------------
// Event queue helpers (caller must hold usb_mutex)
//------------------------------------------------------------------------------------------------

static void queue_init(UsbEventQueue* q)
{
    memset(q, 0, sizeof(*q));
}

static void queue_push(UsbEventQueue* q, const char* name, const char* path)
{
    if (q->count >= USB_EVENT_QUEUE_LEN) {
        // Queue is full - drop the oldest event to make room. This should
        // never happen in practice (MAX_USB_CHANNELS * 2 slots) but we fail
        // gracefully rather than overflowing.
        fprintf(stderr, "WARNING: USB event queue full, dropping oldest event\n");
        q->head = (q->head + 1) % USB_EVENT_QUEUE_LEN;
        q->count--;
    }

    NamePathStruct* slot = &q->events[q->tail];
    safe_copy(slot->device_name, STRING_LEN, name);
    safe_copy(slot->device_path, STRING_LEN, path);

    q->tail = (q->tail + 1) % USB_EVENT_QUEUE_LEN;
    q->count++;
}

static bool queue_pop(UsbEventQueue* q, char* name_out, char* path_out)
{
    if (q->count == 0) {
        return false;
    }

    const NamePathStruct* slot = &q->events[q->head];
    if (name_out) safe_copy(name_out, STRING_LEN, slot->device_name);
    if (path_out) safe_copy(path_out, STRING_LEN, slot->device_path);

    q->head = (q->head + 1) % USB_EVENT_QUEUE_LEN;
    q->count--;
    return true;
}


//------------------------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------------------------

bool device_is_loaded(char* device_name)
{
    if (!device_name) return false;

    pthread_mutex_lock(&usb_mutex);
    bool found = false;
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
        if (strcmp(usb_devices_loaded[i].device_name, device_name) == 0) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&usb_mutex);
    return found;
}


// Returns -1 if the path is invalid
int get_device_id_from_path(const SharedDataStruct* sdp, const char* path)
{
    if (!sdp || !path) return -1;

    for (int device_id = 0; device_id < MAX_USB_CHANNELS; device_id++) {
        const ChannelInfoStruct* channel_info_p = &sdp->channel_info[device_id];
        if (strcmp(channel_info_p->device_path, path) == 0) {
            return device_id;
        }
    }
    return -1;
}


//------------------------------------------------------------------------------------------------
// List of usb devices currently loaded
// (Used to detect insertions or removals)
// Caller must hold usb_mutex.
//------------------------------------------------------------------------------------------------

// Adds a new usb name and path to the list of loaded devices,
// or updates the name of an existing path (should rarely happen).
// If it's a new device, it adds it to the first vacant slot.
static void add_to_devices_loaded_list(const char* name, const char* path)
{
    printf("Entering add_to_devices_loaded_list(%s, %s)\n", name, path);

    // Existing path? Just update the name.
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
        if (strcmp(usb_devices_loaded[i].device_path, path) == 0) {
            safe_copy(usb_devices_loaded[i].device_name, STRING_LEN, name);
            printf("Updated device name in slot %d\n", i);
            return;
        }
    }

    // Path not found. Save in first empty slot.
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
        if (usb_devices_loaded[i].device_path[0] == '\0') {
            safe_copy(usb_devices_loaded[i].device_name, STRING_LEN, name);
            safe_copy(usb_devices_loaded[i].device_path, STRING_LEN, path);
            return;
        }
    }

    fprintf(stderr, "ERROR: No empty slots in usb_devices_loaded[]\n");
    exit(1);
}


// Returns the slot index of `path` in usb_devices_loaded, or -1 if not present.
// Caller must hold usb_mutex.
static int find_in_devices_loaded_list(const char* path)
{
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
        if (strcmp(usb_devices_loaded[i].device_path, path) == 0) {
            return i;
        }
    }
    return -1;
}


//------------------------------------------------------------------------------------------------
// Monitor USB Drives thread
//------------------------------------------------------------------------------------------------

/**
 * @brief Thread function to monitor USB drive insertions and removals.
 * @param arg Unused.
 * @return NULL.
 */
void* monitor_usb_drives_thread_function(void* arg)
{
    (void)arg;

    printf("Monitoring for USB drive events in thread...\n");
    char link_path[PATH_LEN];
    char path[PATH_LEN];
    char buff[STRING_LEN];
    NamePathStruct usb_devices[MAX_USB_CHANNELS]; // list of USB drives actually present
    int usb_devices_count;
    struct dirent* ent;

    while (!usb_monitor_stop) {

        // ----- Scan /sys/block for currently-present removable devices -----

        DIR* dir = opendir("/sys/block");
        if (dir == NULL) {
            perror("Failed to open /sys/block");
            // Don't exit the whole process - just wait and retry.
            usleep(USB_POLL_INTERVAL_US);
            continue;
        }

        memset(usb_devices, 0, sizeof(usb_devices));
        usb_devices_count = 0;

        while ((ent = readdir(dir)) != NULL) {

            // Only interested in sdX devices
            if (strncmp(ent->d_name, "sd", 2) != 0) continue;

            // Check if the device is removable
            snprintf(path, sizeof(path), "/sys/block/%s/removable", ent->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;

            bool removable = false;
            if (fgets(buff, sizeof(buff), f) != NULL && atoi(buff) == 1) {
                removable = true;
            }
            fclose(f);
            if (!removable) continue;

            // Only now that we've accepted this device do we check capacity.
            // (Checking earlier would count non-sd entries like loop/mmcblk.)
            if (usb_devices_count >= MAX_USB_CHANNELS) {
                fprintf(stderr, "WARNING: more than %d removable sd* devices found; ignoring extras\n",
                        MAX_USB_CHANNELS);
                break;
            }

            // Resolve the symbolic link to get the USB port path
            snprintf(path, sizeof(path), "/sys/block/%s", ent->d_name);
            ssize_t len = readlink(path, link_path, sizeof(link_path) - 1);
            if (len == -1) {
                perror("Failed to read symbolic link");
                continue;
            }
            link_path[len] = '\0';

            extract_usb_path(link_path, path);

            // Fill in this slot with bounded, null-terminating copies
            safe_copy_prefixed(usb_devices[usb_devices_count].device_name,
                               STRING_LEN, "/dev/", ent->d_name);
            safe_copy(usb_devices[usb_devices_count].device_path,
                      STRING_LEN, path);
            usb_devices_count++;
        }

        closedir(dir);

        // ----- Compare against usb_devices_loaded to detect changes -----
        //
        // Everything below touches shared state, so hold the mutex for the
        // whole diff. The critical section is bounded by MAX_USB_CHANNELS^2
        // string comparisons, which is trivial.

        pthread_mutex_lock(&usb_mutex);

        // Insertions: entries in usb_devices that aren't in usb_devices_loaded
        for (int i = 0; i < usb_devices_count; i++) {
            if (find_in_devices_loaded_list(usb_devices[i].device_path) < 0) {
                add_to_devices_loaded_list(usb_devices[i].device_name,
                                           usb_devices[i].device_path);

                queue_push(&inserted_queue,
                           usb_devices[i].device_name,
                           usb_devices[i].device_path);

                if (shared_data_p) {
                    int device_id = get_device_id_from_path(shared_data_p,
                                                            usb_devices[i].device_path);
                    printf("add usb device. path=%s, device_id=%d\n",
                           usb_devices[i].device_path, device_id);

                    if (device_id >= 0) {
                        ChannelInfoStruct* client_info_p =
                            &shared_data_p->channel_info[device_id];
                        client_info_p->state = READY;
                        safe_copy(client_info_p->device_name, STRING_LEN,
                                  usb_devices[i].device_name);
                    }
                }
            }
        }

        // Removals: entries in usb_devices_loaded that aren't in usb_devices
        for (int i = 0; i < MAX_USB_CHANNELS; i++) {
            if (usb_devices_loaded[i].device_path[0] == '\0') continue;

            bool found = false;
            for (int j = 0; j < usb_devices_count; j++) {
                if (strcmp(usb_devices[j].device_path,
                           usb_devices_loaded[i].device_path) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) continue;

            // Device is no longer present.
            int device_id = -1;
            if (shared_data_p) {
                device_id = get_device_id_from_path(shared_data_p,
                                                    usb_devices_loaded[i].device_path);
            }
            printf("Removed usb device. id=%d, path=%s\n",
                   device_id, usb_devices_loaded[i].device_path);

            queue_push(&removed_queue,
                       usb_devices_loaded[i].device_name,
                       usb_devices_loaded[i].device_path);

            // Remove from the loaded list
            usb_devices_loaded[i].device_name[0] = '\0';
            usb_devices_loaded[i].device_path[0] = '\0';

            if (shared_data_p && device_id >= 0) {
                ChannelInfoStruct* client_info_p =
                    &shared_data_p->channel_info[device_id];

                // Preserve FAILED/CRC_FAILED so the operator can see which
                // slot failed even after the stick is pulled.
                if (client_info_p->state != FAILED &&
                    client_info_p->state != CRC_FAILED) {
                    client_info_p->state = EMPTY;
                    client_info_p->device_name[0] = '\0';
                }
            }
        }

        pthread_mutex_unlock(&usb_mutex);

        usleep(USB_POLL_INTERVAL_US);
    }

    return NULL;
}


//------------------------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------------------------

// If a USB device has been inserted, copies the name and path into the
// supplied buffers and returns true. Returns false if no insertion events
// are queued. Each event is returned exactly once - call repeatedly to drain.
bool usb_device_inserted(char* name, char* path)
{
    pthread_mutex_lock(&usb_mutex);
    bool got = queue_pop(&inserted_queue, name, path);
    pthread_mutex_unlock(&usb_mutex);

    if (!got) {
        if (name) *name = '\0';
        if (path) *path = '\0';
    }
    return got;
}


// If a USB device has been removed, copies the name and path into the
// supplied buffers and returns true. Returns false if no removal events
// are queued. Each event is returned exactly once - call repeatedly to drain.
bool usb_device_removed(char* name, char* path)
{
    pthread_mutex_lock(&usb_mutex);
    bool got = queue_pop(&removed_queue, name, path);
    pthread_mutex_unlock(&usb_mutex);

    if (!got) {
        if (name) *name = '\0';
        if (path) *path = '\0';
    }
    return got;
}


void usb_init(SharedDataStruct* shared_data)
{
    if (!shared_data) {
        fprintf(stderr, "ERROR: Invalid shared_data passed to usb_init\n");
        exit(1);
    }

    pthread_mutex_lock(&usb_mutex);
    shared_data_p = shared_data;
    memset(usb_devices_loaded, 0, sizeof(usb_devices_loaded));
    queue_init(&inserted_queue);
    queue_init(&removed_queue);
    usb_monitor_stop = false;
    pthread_mutex_unlock(&usb_mutex);

    // Start the USB monitor thread
    if (pthread_create(&usb_monitor_thread, NULL,
                       monitor_usb_drives_thread_function, NULL) != 0) {
        perror("Failed to create USB monitor thread");
        exit(1);
    }
}


void usb_cleanup(void)
{
    usb_monitor_stop = true;

    // Join the thread so it's not leaked. Worst case we wait one poll interval.
    pthread_join(usb_monitor_thread, NULL);

    pthread_mutex_lock(&usb_mutex);
    shared_data_p = NULL;
    queue_init(&inserted_queue);
    queue_init(&removed_queue);
    pthread_mutex_unlock(&usb_mutex);
}

