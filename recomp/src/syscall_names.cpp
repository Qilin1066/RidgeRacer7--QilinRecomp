#include "syscall_names.hpp"

#include <algorithm>

namespace qilin {

namespace {

constexpr SystemCallName kNames[] = {
    {43, "sys_ppu_thread_yield", true},
    {44, "sys_ppu_thread_join", true},
    {45, "sys_ppu_thread_detach", true},
    {47, "sys_ppu_thread_set_priority", true},
    {48, "sys_ppu_thread_get_priority", true},
    {70, "sys_timer_create", false},
    {71, "sys_timer_destroy", false},
    {73, "_sys_timer_start", false},
    {74, "sys_timer_stop", false},
    {75, "sys_timer_connect_event_queue", false},
    {76, "sys_timer_disconnect_event_queue", false},
    {90, "sys_semaphore_create", true},
    {91, "sys_semaphore_destroy", true},
    {92, "sys_semaphore_wait", true},
    {94, "sys_semaphore_post", true},
    {100, "sys_mutex_create", true},
    {101, "sys_mutex_destroy", true},
    {102, "sys_mutex_lock", true},
    {103, "sys_mutex_trylock", true},
    {104, "sys_mutex_unlock", true},
    {105, "sys_cond_create", true},
    {106, "sys_cond_destroy", true},
    {107, "sys_cond_wait", true},
    {108, "sys_cond_signal", true},
    {114, "sys_semaphore_get_value", true},
    {128, "sys_event_queue_create", true},
    {129, "sys_event_queue_destroy", true},
    {130, "sys_event_queue_receive", true},
    {134, "sys_event_port_create", true},
    {135, "sys_event_port_destroy", true},
    {136, "sys_event_port_connect_local", true},
    {137, "sys_event_port_disconnect", true},
    {138, "sys_event_port_send", true},
    {141, "sys_timer_usleep", true},
    {142, "sys_timer_sleep", true},
    {145, "sys_time_get_current_time", true},
    {147, "sys_time_get_timebase_frequency", true},
    {169, "sys_spu_initialize", true},
    {170, "sys_spu_thread_group_create", true},
    {171, "sys_spu_thread_group_destroy", true},
    {172, "sys_spu_thread_initialize", true},
    {173, "sys_spu_thread_group_start", true},
    {178, "sys_spu_thread_group_join", true},
    {181, "sys_spu_thread_write_ls", true},
    {182, "sys_spu_thread_read_ls", true},
    {190, "sys_spu_thread_write_spu_mb", true},
    {191, "sys_spu_thread_connect_event", true},
    {192, "sys_spu_thread_disconnect_event", true},
    {193, "sys_spu_thread_bind_queue", true},
    {194, "sys_spu_thread_unbind_queue", true},
    {341, "sys_memory_container_create", false},
    {342, "sys_memory_container_destroy", false},
    {348, "sys_memory_allocate", true},
    {349, "sys_memory_free", true},
    {352, "sys_memory_get_user_memory_size", false},
    {402, "sys_tty_read", true},
    {403, "sys_tty_write", true},
    {801, "sys_fs_open", true},
    {802, "sys_fs_read", true},
    {803, "sys_fs_write", true},
    {804, "sys_fs_close", true},
    {805, "sys_fs_opendir", true},
    {806, "sys_fs_readdir", true},
    {807, "sys_fs_closedir", true},
    {808, "sys_fs_stat", true},
    {809, "sys_fs_fstat", true},
    {810, "sys_fs_link", true},
    {811, "sys_fs_mkdir", true},
    {812, "sys_fs_rename", true},
    {813, "sys_fs_rmdir", true},
    {814, "sys_fs_unlink", true},
    {815, "sys_fs_utime", true},
    {817, "sys_fs_fcntl", false},
    {818, "sys_fs_lseek", true},
    {820, "sys_fs_fsync", true},
    {831, "sys_fs_truncate", true},
    {832, "sys_fs_ftruncate", true},
    {834, "sys_fs_chmod", true},
};

}  // namespace

std::span<const SystemCallName> system_call_names() { return kNames; }

const SystemCallName* system_call_name(uint32_t number) {
    const auto it = std::ranges::lower_bound(kNames, number, {}, &SystemCallName::number);
    return it != std::end(kNames) && it->number == number ? &*it : nullptr;
}

}  // namespace qilin
