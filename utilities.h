#ifndef UTILITIES_H
#define UTILITIES_H


const char* get_state_name(const ChannelStateEnum state);

int execute_command(const int device_id, const char *cmd, const bool ignore_errors);

uint64_t get_directory_size(const char *path);

int copy_file(const char *src_path, const char *dest_path, bool *halt_p, off_t *bytes_copied_p);

int copy_directory(const char *src_dir, const char *dest_dir, bool* halt_p, off_t *bytes_copied_p);

void usleep(const uint64_t usec);

void print_shared_data(SharedDataStruct* shared_data_p);

void extract_usb_path(const char *input, char *output);	

int get_device_id_from_path(const SharedDataStruct* shared_data_p, char* path);
	
#endif // UTILITIES_H


