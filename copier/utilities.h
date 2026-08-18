#ifndef UTILITIES_H
#define UTILITIES_H


void trim(char *str);

const char* get_state_name(const ChannelStateEnum state);

int execute_command(const int device_id, const char *cmd, const bool ignore_errors);

uint64_t get_directory_size(const char *path);

int copy_file(const char *src_path, const char *dest_path, volatile bool *halt_p, off_t *bytes_copied_p);

int copy_directory(const char *src_dir, const char *dest_dir, volatile bool* halt_p, off_t *bytes_copied_p);

// Optional hook, called with the destination name of each file just before it is copied.
// NULL (the default) disables it. The server uses this to show the current file on the
// LCD; the client leaves it unset, which keeps utilities.c free of any LCD dependency.
void set_copy_progress_callback(void (*callback)(const char *filename));

void print_shared_data(const SharedDataStruct* shared_data_p);

int get_device_id_from_hub_and_port_number(const SharedDataStruct* shared_data_p, int hub_number, int port_number);

void initialise_crc_table();

uint32_t compute_crc32(char *filename);

void shorten_filename(char *filename, size_t max_len);

void sanitize_filename(char *filename);

bool get_volume_label(const char *partition_name, char *label_out, size_t label_out_size);


#endif // UTILITIES_H


