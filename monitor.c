#include <gtk/gtk.h>
#include <gtk/gtktreeviewcolumn.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* ================= 列定义 ================= */
enum {
    COL_PID,
    COL_NAME,
    COL_CPU,
    COL_MEM,
    COL_DISK,
    NUM_COLS
};

GtkListStore* store;
GtkCellRendererText* renderers[NUM_COLS]; // 保存每列的渲染器

/* ================= CPU Total ================= */
typedef struct {
    long long total;
    long long idle;
} CpuTotal;

/* ================= Disk Total ================= */
typedef struct {
    long long sectors;
} DiskTotal;

/* ================= 每个进程历史记录 ================= */
typedef struct {
    long long utime, stime;
} ProcCpu;

typedef struct {
    long long read_bytes, write_bytes;
} ProcIO;

GHashTable* cpu_table; // key: pid(int*), value: ProcCpu*
GHashTable* io_table;  // key: pid(int*), value: ProcIO*

/* ================= 全局变量 ================= */
GtkWidget* process_panel_box;  // 放在 Stack 中的进程面板 Box
GtkWidget* process_tree_view;  // 进程列表 TreeView
GtkWidget* performance_panel;  // 性能面板
GtkWidget* search_entry;       // 搜索框
GtkTreeModelFilter* filter_model; // 过滤模型
GtkTreeModelSort* sort_model;     // 排序模型

static int current_sort_col = COL_PID;   // 当前排序列
//static GtkTreeRowReference* selected_ref = NULL; //选择行
static int selected_pid = -1;            // 选中进程pid
static int flash_time = 2;               // 刷新时间 单位秒
static char search_text[128] = "";       // 搜索文本框

/* ================= 工具函数 ================= */
int is_pid_dir(const char* name) 
{
    for (int i = 0; name[i]; i++)
        if (!isdigit(name[i]))
            return 0;
    return 1;
}

void get_process_name(const char* pid, char* buf, size_t size) 
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%s/comm", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) {
        strncpy(buf, "unknown", size);
        return;
    }
    fgets(buf, size, fp);
    buf[strcspn(buf, "\n")] = 0;
    fclose(fp);
}

/* 搜索框逻辑 */
void on_search_changed(GtkEntry* entry, gpointer user_data) 
{
    const char* text = gtk_entry_get_text(entry);
    g_strlcpy(search_text, text, sizeof(search_text));
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
}

/* 模糊搜索函数 */
gboolean filter_visible_func(GtkTreeModel* model, GtkTreeIter* iter, gpointer data) 
{
    if (search_text[0] == '\0') return TRUE;

    char buf[128] = { 0 };

    if (current_sort_col == COL_NAME)//名字字符串模糊搜索
    {
        gchar* name = NULL;
        gtk_tree_model_get(model, iter, COL_NAME, &name, -1);

        if (name) {
            g_strlcpy(buf, name, sizeof(buf));
            g_free(name);
        }
        else {
            buf[0] = '\0';
        }
    }

    else if (current_sort_col == COL_PID) //pid整形模糊搜索
    {
        int pid;
        gtk_tree_model_get(model, iter, COL_PID, &pid, -1);
        snprintf(buf, sizeof(buf), "%d", pid);
    }
    else //cpu,mem,io浮点模糊搜索
    {
        double val;
        gtk_tree_model_get(model, iter, current_sort_col, &val, -1);
        snprintf(buf, sizeof(buf), "%.2f", val);
    }

    return g_strrstr(buf, search_text) != NULL;
}

void on_btn_process_clicked_stack(GtkButton* button, gpointer user_data) 
{
    GtkWidget* stack = GTK_WIDGET(user_data);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "process");
}

void on_btn_performance_clicked_stack(GtkButton* button, gpointer user_data) 
{
    GtkWidget* stack = GTK_WIDGET(user_data);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "performance");
}

void column_clicked(GtkTreeViewColumn* col, gpointer user_data) 
{
    int col_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "col_index"));
    current_sort_col = col_index;

    if (filter_model) 
    {
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
    }

    for (int i = 0; i < NUM_COLS; i++) 
    {
        GdkRGBA color;
        if (i == col_index)
            gdk_rgba_parse(&color, "#e0e0e0"); // 选中列
        else
            gdk_rgba_parse(&color, "white");
        g_object_set(renderers[i], "cell-background-rgba", &color, NULL);
    }
}

/* ================= 系统 CPU ================= */
CpuTotal get_cpu_total() 
{
    FILE* fp = fopen("/proc/stat", "r");
    CpuTotal c = { 0 };
    if (!fp) return c;

    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
            &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
        c.idle = idle + iowait;
        c.total = user + nice + sys + c.idle + irq + softirq + steal;
    }
    fclose(fp);
    return c;
}

/* ================= 系统内存 ================= */
long long get_mem_total_kb() 
{
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;
    char line[128];
    long long total = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lld kB", &total) == 1)
            break;
    }
    fclose(fp);
    return total;
}

double get_mem_percent() 
{
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0;

    long long total = 0, avail = 0;
    char line[128], key[64];
    long long val;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%63s %lld kB", key, &val) != 2) continue;
        if (strcmp(key, "MemTotal:") == 0) total = val;
        else if (strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    fclose(fp);
    return total ? 100.0 * (total - avail) / total : 0.0;
}

/* ================= 系统磁盘 ================= */
DiskTotal get_disk_total() 
{
    FILE* fp = fopen("/proc/diskstats", "r");
    DiskTotal d = { 0 };
    if (!fp) return d;

    char line[256];
    while (fgets(line, sizeof(line), fp)) 
    {
        int major = 0, minor = 0;
        char name[32] = { 0 };
        long long tmp1, tmp2, rd_sectors, tmp4, tmp5, tmp6, wr_sectors, tmp8;
        if (sscanf(line, "%d %d %31s %lld %lld %lld %lld %lld %lld %lld %lld",
            &major, &minor, name, &tmp1, &tmp2, &rd_sectors, &tmp4,
            &tmp5, &tmp6, &wr_sectors, &tmp8) != 11)
            continue;
        if (strncmp(name, "sd", 2) == 0 || strncmp(name, "nvme", 4) == 0)
            d.sectors += rd_sectors + wr_sectors;
    }
    fclose(fp);
    return d;
}

/* ================= 进程 CPU ================= */
int get_proc_cpu(int pid, ProcCpu* pc) 
{
    char path[128], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);

    char comm[256], state;
    sscanf(buf, "%*d %s %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld",
        comm, &state, &pc->utime, &pc->stime);
    return 1;
}

/* ================= 进程内存 ================= */
double get_proc_mem(int pid, long long mem_total) 
{
    char path[128], line[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0.0;

    long long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "VmRSS: %lld kB", &rss) == 1) break;
    }
    fclose(fp);
    return mem_total ? 100.0 * rss / mem_total : 0.0;
}

/* ================= 进程 I/O ================= */
int get_proc_io(int pid, ProcIO* io) 
{
    char path[128], line[128];
    snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    io->read_bytes = io->write_bytes = 0;
    while (fgets(line, sizeof(line), fp)) 
    {
        long long tmp;
        if (sscanf(line, "read_bytes: %lld", &tmp) == 1) io->read_bytes = tmp;
        if (sscanf(line, "write_bytes: %lld", &tmp) == 1) io->write_bytes = tmp;
    }
    fclose(fp);
    return 1;
}

/* ================= 排序函数 ================= */
gint sort_func(GtkTreeModel* model, GtkTreeIter* a, GtkTreeIter* b, gpointer data) 
{
    int col = GPOINTER_TO_INT(data);
    double va, vb;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    if (va < vb) return 1;
    if (va > vb) return -1;
    return 0;
}

gint sort_name_func(GtkTreeModel* model, GtkTreeIter* a, GtkTreeIter* b, gpointer data) 
{
    gchar* name_a; gchar* name_b;
    gtk_tree_model_get(model, a, COL_NAME, &name_a, -1);
    gtk_tree_model_get(model, b, COL_NAME, &name_b, -1);
    gint ret = g_strcmp0(name_a, name_b);
    g_free(name_a); g_free(name_b);
    return ret;
}

/* ================= 结束任务 ================= */
void on_kill_task_clicked(GtkButton* button, gpointer user_data) 
{ 
    if (selected_pid <= 0) 
    { 
        GtkWidget* dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "请先选择一个进程");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg); return; } 
    if (kill(selected_pid, SIGTERM) == 0) 
    {
        g_print("已向 PID %d 发送 SIGTERM\n", selected_pid); 
    } 
    else 
    { 
        GtkWidget* dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "无法结束该进程（权限不足或受保护进程）");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
    } 
}

/* ================= 进程列表更新 ================= */
gboolean update_process_list(gpointer data)
{

    // 保存选中行 PID
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(process_tree_view));
    GtkTreeModel* model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_tree_model_get(model, &iter, COL_PID, &selected_pid, -1);
    }

    // 清空列表
    gtk_list_store_clear(store);

    CpuTotal cur_cpu = get_cpu_total();
    long long mem_total = get_mem_total_kb();

    DIR* dir = opendir("/proc");
    if (!dir) return TRUE;

    struct dirent* e;
    GtkTreeIter new_iter;

    while ((e = readdir(dir))) {
        if (!is_pid_dir(e->d_name)) continue;

        int pid = atoi(e->d_name);
        char name[128];
        get_process_name(e->d_name, name, sizeof(name));

        // CPU
        ProcCpu pc = { 0 };
        if (!get_proc_cpu(pid, &pc)) continue;

        int key = pid;
        ProcCpu* prev = g_hash_table_lookup(cpu_table, &key);
        double cpu = 0.0;
        if (prev) {
            long long delta = (pc.utime + pc.stime) - (prev->utime + prev->stime);
            long long total_diff = cur_cpu.total;
            if (total_diff > 0)
                cpu = 100.0 * delta / total_diff;

            prev->utime = pc.utime;
            prev->stime = pc.stime;
        }
        else {
            ProcCpu* val = malloc(sizeof(ProcCpu));
            *val = pc;
            int* k = malloc(sizeof(int));
            *k = pid;
            g_hash_table_insert(cpu_table, k, val);
        }

        // MEM
        double mem = get_proc_mem(pid, mem_total);

        // IO
        ProcIO io = { 0 };
        double io_kb = 0.0;
        if (get_proc_io(pid, &io)) {
            ProcIO* prev_io = g_hash_table_lookup(io_table, &key);
            if (prev_io) {
                io_kb = ((io.read_bytes - prev_io->read_bytes) +
                    (io.write_bytes - prev_io->write_bytes)) / 1024.0;
                prev_io->read_bytes = io.read_bytes;
                prev_io->write_bytes = io.write_bytes;
            }
            else {
                ProcIO* val = malloc(sizeof(ProcIO));
                *val = io;
                int* k = malloc(sizeof(int));
                *k = pid;
                g_hash_table_insert(io_table, k, val);
            }
        }

        // 添加到列表
        gtk_list_store_append(store, &new_iter);
        gtk_list_store_set(store, &new_iter,
            COL_PID, pid,
            COL_NAME, name,
            COL_CPU, cpu,
            COL_MEM, mem,
            COL_DISK, io_kb,
            -1);
    }

    closedir(dir);

    // 恢复选中行（基于 PID 查找）
    if (selected_pid != -1) {
        GtkTreeIter store_iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &store_iter);
        while (valid) {
            gint pid;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &store_iter, COL_PID, &pid, -1);
            if (pid == selected_pid) {
                // store -> filter
                GtkTreePath* filter_path = gtk_tree_model_filter_convert_child_path_to_path(
                    GTK_TREE_MODEL_FILTER(filter_model),
                    gtk_tree_model_get_path(GTK_TREE_MODEL(store), &store_iter)
                );

                // filter -> sort
                GtkTreePath* sort_path = gtk_tree_model_sort_convert_child_path_to_path(
                    GTK_TREE_MODEL_SORT(sort_model),
                    filter_path
                );

                if (sort_path) {
                    GtkTreeSelection* sel =
                        gtk_tree_view_get_selection(GTK_TREE_VIEW(process_tree_view));
                    gtk_tree_selection_select_path(sel, sort_path);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(process_tree_view),
                        sort_path,
                        NULL, FALSE, 0, 0);

                    gtk_tree_path_free(sort_path);
                }

                gtk_tree_path_free(filter_path);
                break;
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &store_iter);
        }
    }

    return TRUE;
}

/* ================= 系统状态刷新 ================= */
gboolean update_system_total(gpointer labels) 
{
    GtkWidget* label = GTK_WIDGET(labels);

    CpuTotal cur_cpu = get_cpu_total();
    DiskTotal cur_disk = get_disk_total();
    static CpuTotal prev_cpu = { 0 };
    static DiskTotal prev_disk = { 0 };

    double cpu_p = 0.0, disk_kb = 0.0;

    if (prev_cpu.total > 0) {
        long long total_diff = cur_cpu.total - prev_cpu.total;
        long long idle_diff = cur_cpu.idle - prev_cpu.idle;
        if (total_diff > 0)
            cpu_p = 100.0 * (1.0 - (double)idle_diff / total_diff);
    }

    if (prev_disk.sectors > 0) {
        long long sec_diff = cur_disk.sectors - prev_disk.sectors;
        disk_kb = sec_diff * 512.0 / 1024.0; // 转换为 KB/s
    }

    prev_cpu = cur_cpu;
    prev_disk = cur_disk;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "System Total | CPU: %.1f%% | MEM: %.1f%% | Disk: %.1f KB/s",
        cpu_p, get_mem_percent(), disk_kb);
    gtk_label_set_text(GTK_LABEL(label), buf);

    return TRUE;
}

//进程面板渲染
GtkWidget* create_process_panel()
{
    process_panel_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    // 系统状态标签
    GtkWidget* sys_label = gtk_label_new("System Total | CPU: 0% | MEM: 0% | Disk: 0 KB/s");
    gtk_box_pack_start(GTK_BOX(process_panel_box), sys_label, FALSE, FALSE, 5);

    // 创建 ListStore
    store = gtk_list_store_new(NUM_COLS,
        G_TYPE_INT,
        G_TYPE_STRING,
        G_TYPE_DOUBLE,
        G_TYPE_DOUBLE,
        G_TYPE_DOUBLE);

    // 模糊搜索模型
    filter_model = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_func(filter_model, filter_visible_func, NULL, NULL);

    // 排序模型
    sort_model = GTK_TREE_MODEL_SORT(gtk_tree_model_sort_new_with_model(GTK_TREE_MODEL(filter_model)));
    process_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sort_model));

    // 滚动窗口
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), process_tree_view);
    gtk_box_pack_start(GTK_BOX(process_panel_box), scroll, TRUE, TRUE, 0);

    // ------------------ 底部搜索 + 结束任务 ------------------
    GtkWidget* bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    // 左侧搜索框
    GtkWidget* search_label = gtk_label_new("搜索：");
    search_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(bottom_box), search_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom_box), search_entry, TRUE, TRUE, 0);
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);

    // 右侧结束任务按钮
    GtkWidget* kill_btn = gtk_button_new_with_label("结束任务");
    g_signal_connect(kill_btn, "clicked", G_CALLBACK(on_kill_task_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bottom_box), kill_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(process_panel_box), bottom_box, FALSE, FALSE, 5);

    // 列标题及渲染器
    const char* titles[NUM_COLS] = { "PID", "Name", "CPU%", "MEM%", "Disk KB/s" };
    for (int i = 0; i < NUM_COLS; i++) {
        GtkTreeViewColumn* col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, titles[i]);
        renderers[i] = GTK_CELL_RENDERER_TEXT(gtk_cell_renderer_text_new());
        gtk_tree_view_column_pack_start(col, GTK_CELL_RENDERER(renderers[i]), TRUE);
        gtk_tree_view_column_add_attribute(col, GTK_CELL_RENDERER(renderers[i]), "text", i);
        gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), col);

        g_object_set_data(G_OBJECT(col), "col_index", GINT_TO_POINTER(i));
        g_signal_connect(col, "clicked", G_CALLBACK(column_clicked), NULL);
        gtk_tree_view_column_set_sort_column_id(col, i);
    }

    // 定时刷新
    g_timeout_add_seconds(flash_time, update_process_list, NULL);
    g_timeout_add_seconds(flash_time, update_system_total, sys_label);

    return process_panel_box;
}

// 创建性能面板
GtkWidget* create_performance_panel()
{
    GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* perf_label = gtk_label_new("性能面板（可以放 CPU、内存、磁盘曲线图）");
    gtk_box_pack_start(GTK_BOX(panel), perf_label, TRUE, TRUE, 0);

    return panel;
}


/* ================= 主函数 ================= */
int main(int argc, char* argv[])
{
    gtk_init(&argc, &argv);

    cpu_table = g_hash_table_new(g_int_hash, g_int_equal);
    io_table = g_hash_table_new(g_int_hash, g_int_equal);

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Linux任务管理器");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 500);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);

    // 左侧按钮
    GtkWidget* left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(left_box, 100, -1);
    gtk_box_pack_start(GTK_BOX(hbox), left_box, FALSE, FALSE, 0);

    GtkWidget* btn_process = gtk_button_new_with_label("进程");
    GtkWidget* btn_performance = gtk_button_new_with_label("性能");
    gtk_box_pack_start(GTK_BOX(left_box), btn_process, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), btn_performance, FALSE, FALSE, 0);

    // 右侧 Stack
    GtkWidget* stack = gtk_stack_new();
    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);

    // 渲染进程面板
    GtkWidget* process_panel = create_process_panel();
    gtk_stack_add_named(GTK_STACK(stack), process_panel, "process");

    // 性能面板
    performance_panel = create_performance_panel();
    gtk_stack_add_named(GTK_STACK(stack), performance_panel, "performance");

    // 默认显示进程面板
    gtk_stack_set_visible_child(GTK_STACK(stack), process_panel);

    // 面板切换
    g_signal_connect(btn_process, "clicked",G_CALLBACK(on_btn_process_clicked_stack), stack);
    g_signal_connect(btn_performance, "clicked",G_CALLBACK(on_btn_performance_clicked_stack), stack);

    gtk_widget_show_all(win);
    gtk_main();

    return 0;
}
