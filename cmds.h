/*
 * Author: Kent Overstreet <kent.overstreet@gmail.com>
 *
 * GPLv2
 */

#ifndef _CMDS_H
#define _CMDS_H

#include "tools-util.h"

int cmd_format(int argc, char *argv[]);
int cmd_show_super(int argc, char *argv[]);
int cmd_set_option(int argc, char *argv[]);

#if 0
int cmd_assemble(int argc, char *argv[]);
int cmd_incremental(int argc, char *argv[]);
int cmd_run(int argc, char *argv[]);
int cmd_stop(int argc, char *argv[]);
#endif

int cmd_fs_usage(int argc, char *argv[]);

int device_usage(void);
int cmd_device_add(int argc, char *argv[]);
int cmd_device_remove(int argc, char *argv[]);
int cmd_device_online(int argc, char *argv[]);
int cmd_device_offline(int argc, char *argv[]);
int cmd_device_evacuate(int argc, char *argv[]);
int cmd_device_set_state(int argc, char *argv[]);
int cmd_device_resize(int argc, char *argv[]);
int cmd_device_resize_journal(int argc, char *argv[]);

int data_usage(void);
int cmd_data_rereplicate(int argc, char *argv[]);
int cmd_data_job(int argc, char *argv[]);

int cmd_unlock(int argc, char *argv[]);
int cmd_set_passphrase(int argc, char *argv[]);
int cmd_remove_passphrase(int argc, char *argv[]);

int cmd_fsck(int argc, char *argv[]);

int cmd_dump(int argc, char *argv[]);
int cmd_list(int argc, char *argv[]);
int cmd_list_journal(int argc, char *argv[]);
int cmd_kill_btree_node(int argc, char *argv[]);

int cmd_migrate(int argc, char *argv[]);
int cmd_migrate_superblock(int argc, char *argv[]);

int cmd_version(int argc, char *argv[]);

int cmd_setattr(int argc, char *argv[]);

int cmd_attr(int argc, char *argv[]);

int subvolume_usage(void);
int cmd_subvolume_create(int argc, char *argv[]);
int cmd_subvolume_delete(int argc, char *argv[]);
int cmd_subvolume_snapshot(int argc, char *argv[]);

int cmd_fusemount(int argc, char *argv[]);
void cmd_mount(int agc, char *argv[]);

#endif /* _CMDS_H */
