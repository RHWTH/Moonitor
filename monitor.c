#include <gtk/gtk.h>
#include <gtk/gtktreeviewcolumn.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define HISTORY_LEN 60  // 保存 60 个点

typedef struct {
    char model[128];
    int cores;
    int threads;
    int cache_kb;
    double freq_ghz;
    double usage_percent;  // CPU 使用率
} CpuInfo;

typedef struct {
    long long mem_total;
    long long mem_free;
    long long buffers;
    long long cached;
    long long swap_total;
    long long swap_free;
} MemStat;

typedef struct {
    long long total;
    long long idle;
} CpuTotal;

typedef struct {
    long long sectors;
} DiskTotal;

typedef struct {
    long long utime, stime;
} ProcCpu;

typedef struct {
    long long read_bytes, write_bytes;
} ProcIO;

typedef struct {
    double cpu[HISTORY_LEN];
    double mem[HISTORY_LEN];
    double disk[HISTORY_LEN];
    int index; // 下一个写入位置
} PerfData;

typedef enum {
    PERF_CPU,
    PERF_MEM,
    PERF_DISK
} PerfType;

typedef struct {
    unsigned long long read_sectors;
    unsigned long long write_sectors;
    unsigned long long busy_time;
} DiskStats;

DiskStats last_disk_stats = { 0 };

enum {
    COL_PID,
    COL_NAME,
    COL_CPU,
    COL_MEM,
    COL_DISK,
    NUM_COLS
};

PerfType current_perf = PERF_CPU;

PerfData perf_data = { {0}, {0}, {0}, 0 };
GtkWidget* perf_stack;
GtkWidget* perf_drawing_area;
GtkWidget* perf_cpu_label;
GtkWidget* perf_mem_label;
GtkWidget* perf_disk_label;
GtkWidget* cpu_drawing_area;
GtkWidget* mem_drawing_area;
GtkWidget* disk_drawing_area;

/* ================= 全局变量 ================= */
GtkListStore* store;//读到的进程数据
GtkCellRendererText* renderers[NUM_COLS]; // 保存每列的渲染器
GtkWidget* process_panel_box;  // 放在 Stack 中的进程面板 Box
GtkWidget* process_tree_view;  // 进程列表 TreeView
GtkWidget* performance_panel;  // 性能面板
GtkWidget* search_entry;       // 搜索框
GtkTreeModelFilter* filter_model; // 过滤模型
GtkTreeModelSort* sort_model;     // 排序模型
GHashTable* cpu_table;
GHashTable* io_table;

GtkWidget* cpu_detail_label;//cpu详细信息标签
GtkWidget* mem_info_label = NULL;//内存详细信息标签
GtkWidget* disk_read_label;
GtkWidget* disk_write_label;
GtkWidget* disk_active_label;


int is_selection = 0;//是否保持选中
static int current_sort_col = COL_PID;   // 当前排序列
static int selected_pid = -1;            // 选中进程pid
static int flash_time = 1;               // 刷新时间 单位秒
static char search_text[128] = "";       // 搜索文本框


double cpu_p = 0.0; //当前cpu的总占用
double disk_kb = 0.0;//当前磁盘的总占用
double mem_p = 0.0;//当前内存总占用


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

void on_row_selected(GtkTreeView* treeview, gpointer user_data)
{
    is_selection = 1; // 允许 update_process_list 保持选中
}
/* 搜索框逻辑 */
void on_search_changed(GtkEntry* entry, gpointer user_data) 
{
    const char* text = gtk_entry_get_text(entry);
    g_strlcpy(search_text, text, sizeof(search_text));
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
}

/* ================= 点击效果 ================= */
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

// stack切换
gboolean on_stack_row_clicked(GtkWidget* widget, GdkEventButton* event, gpointer user_data)
{
    gpointer* data = (gpointer*)user_data;
    GtkWidget* stack = GTK_WIDGET(data[0]);
    const char* name = (const char*)data[1];

    gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
    return TRUE;
}

// 创建可点击行
GtkWidget* create_clickable_row(const char* text, GtkWidget* stack, const char* stack_name)
{
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 5);
    gtk_widget_set_margin_top(label, 5);

    GtkWidget* box = gtk_event_box_new(); // EventBox 让 Label 可点击
    gtk_container_add(GTK_CONTAINER(box), label);

    // 用数组传递 stack 和 name
    gpointer* data = g_new(gpointer, 2);
    data[0] = stack;
    data[1] = (gpointer)stack_name;

    g_signal_connect(box, "button-press-event",
        G_CALLBACK(on_stack_row_clicked),
        data);

    return box;
}

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

void column_clicked(GtkTreeViewColumn* col, gpointer user_data) 
{
    int col_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "col_index"));
    current_sort_col = col_index;

    is_selection = 0; // 禁止刷新保持选中
    selected_pid = -1;

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

void on_perf_row_selected(GtkListBox* box, GtkListBoxRow* row, gpointer data)//性能面板不同类型选中逻辑
{
    if (!row) return;

    int type = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(row), "perf_type"));

    switch (type) {
    case PERF_CPU:
        current_perf = PERF_CPU;
        gtk_stack_set_visible_child_name(GTK_STACK(perf_stack), "cpu");
        break;
    case PERF_MEM:
        current_perf = PERF_MEM;
        gtk_stack_set_visible_child_name(GTK_STACK(perf_stack), "mem");
        break;
    case PERF_DISK:
        current_perf = PERF_DISK;
        gtk_stack_set_visible_child_name(GTK_STACK(perf_stack), "disk");
        break;
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
        long long user, nice, sys, idle, iowait, irq, softirq, steal;
        int n = sscanf(line,
            "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
            &user, &nice, &sys, &idle,
            &iowait, &irq, &softirq, &steal);

        if (n >= 7) {
            c.idle = idle + (n > 4 ? iowait : 0);
            c.total = user + nice + sys + c.idle +
                (n > 5 ? irq : 0) +
                (n > 6 ? softirq : 0) +
                (n > 7 ? steal : 0);
        }
    }
    fclose(fp);
    return c;
}

void get_cpu_info(CpuInfo* info)
{
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        strcpy(info->model, "unknown");
        info->cores = 0;
        info->threads = 0;
        info->cache_kb = 0;
        info->freq_ghz = 0.0;
        return;
    }

    char line[256];
    info->cores = 0;
    info->threads = 0;
    info->cache_kb = 0;
    info->model[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "model name", 10) == 0 && info->model[0] == '\0') {
            char* p = strchr(line, ':');
            if (p) {
                p += 2;
                strncpy(info->model, p, sizeof(info->model) - 1);
                info->model[strcspn(info->model, "\n")] = 0;
            }
        }
        else if (strncmp(line, "cpu cores", 9) == 0 && info->cores == 0) {
            int cores = 0;
            sscanf(line, "cpu cores\t: %d", &cores);
            info->cores = cores;
        }
        else if (strncmp(line, "processor", 9) == 0) {
            info->threads++;
        }
        else if (strncmp(line, "cache size", 10) == 0 && info->cache_kb == 0) {
            int cache = 0;
            sscanf(line, "cache size\t: %d", &cache);
            info->cache_kb = cache;
        }
    }
    fclose(fp);

    // ----- 频率读取 fallback -----

    long freq = 0;

    // ① scaling_cur_freq
    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (fp) {
        fscanf(fp, "%ld", &freq);
        fclose(fp);
        info->freq_ghz = freq / 1000000.0;
        return;
    }

    // ② cpuinfo_cur_freq
    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq", "r");
    if (fp) {
        fscanf(fp, "%ld", &freq);
        fclose(fp);
        info->freq_ghz = freq / 1000000.0;
        return;
    }

    // ③ /proc/cpuinfo 的 cpu MHz
    fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "cpu MHz\t: %ld", &freq) == 1) {
                info->freq_ghz = freq / 1000.0;
                fclose(fp);
                return;
            }
        }
        fclose(fp);
    }

    // fallback
    info->freq_ghz = 0.0;
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

int get_mem_stat(MemStat* m)//性能-内存-详细信息获取
{
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;

    char key[64];
    long long value;
    char unit[32];

    memset(m, 0, sizeof(MemStat));

    while (fscanf(fp, "%63s %lld %31s", key, &value, unit) == 3)
    {
        if (strcmp(key, "MemTotal:") == 0) m->mem_total = value;
        else if (strcmp(key, "MemFree:") == 0) m->mem_free = value;
        else if (strcmp(key, "Buffers:") == 0) m->buffers = value;
        else if (strcmp(key, "Cached:") == 0) m->cached = value;
        else if (strcmp(key, "SwapTotal:") == 0) m->swap_total = value;
        else if (strcmp(key, "SwapFree:") == 0) m->swap_free = value;
    }

    fclose(fp);
    return 1;
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

/* ================= 绘图函数 ================= */
void draw_perf_line(cairo_t* cr, double* data_array, int start, int len,int h, double dx, double r, double g, double b, double scale)
{
    cairo_save(cr);
    cairo_move_to(cr, 0, h);
    for (int i = 0; i < len; i++) 
    {
        int idx = (start + i) % len;
        double y = h * (1.0 - data_array[idx] / scale);
        if (y < 0) y = 0;
        cairo_line_to(cr, i * dx, y);
    }
    cairo_line_to(cr, (len - 1) * dx, h);
    cairo_line_to(cr, 0, h);
    cairo_close_path(cr);

    cairo_pattern_t* pat = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, 0.40); // 顶部alpha
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.35); // 底部alpha
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    cairo_restore(cr);

    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, 0, h * (1.0 - data_array[start] / scale));
    for (int i = 0; i < len; i++) {
        int idx = (start + i) % len;
        double y = h * (1.0 - data_array[idx] / scale);
        if (y < 0) y = 0;
        cairo_line_to(cr, i * dx, y);
    }
    cairo_stroke(cr);
}

gboolean draw_performance(GtkWidget* widget, cairo_t* cr, gpointer data)
{
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    /* ===== 背景 ===== */
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_paint(cr);

    double dx = (double)w / (HISTORY_LEN-2);
    int start = (perf_data.index + 1) % HISTORY_LEN;

    /* 线条更圆润 */
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    switch (current_perf) {
    case PERF_CPU:
        draw_perf_line(cr, perf_data.cpu, start, HISTORY_LEN, h, dx, 0.3, 0.6, 1.0, 100.0);
        break;
    case PERF_MEM:
        draw_perf_line(cr, perf_data.mem, start, HISTORY_LEN, h, dx, 0.05, 0.2, 0.6, 100.0);
        break;
    case PERF_DISK:
        draw_perf_line(cr, perf_data.disk, start, HISTORY_LEN, h, dx, 0.3, 0.8, 0.6, 1024.0);
        break;
    }

    return FALSE;
}

/* ================= 进程列表更新 ================= */
gboolean update_process_list(gpointer data)
{
    static CpuTotal prev_cpu = { 0 };

    // 保存选中的 PID
    if (is_selection)
    {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(process_tree_view));
        GtkTreeModel* model;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, &model, &iter))
        {
            gtk_tree_model_get(model, &iter, COL_PID, &selected_pid, -1);
        }
    }
    
    

    // 清空 ListStore
    gtk_list_store_clear(store);

    // 系统 CPU：计算总差值
    CpuTotal cur_cpu = get_cpu_total();
    long long total_diff = 0;

    if (prev_cpu.total > 0)
        total_diff = cur_cpu.total - prev_cpu.total;

    long long mem_total = get_mem_total_kb();
    int ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    DIR* dir = opendir("/proc");
    if (!dir) return TRUE;

    struct dirent* e;
    GtkTreeIter new_iter;

    while ((e = readdir(dir))) {
        if (!is_pid_dir(e->d_name)) continue;

        int pid = atoi(e->d_name);
        char name[128];
        get_process_name(e->d_name, name, sizeof(name));

        // ---- CPU ----
        ProcCpu pc;
        if (!get_proc_cpu(pid, &pc)) continue;

        ProcCpu* prev = g_hash_table_lookup(cpu_table, GINT_TO_POINTER(pid));
        double cpu = 0.0;

        if (prev && total_diff > 0) {
            long long delta = (pc.utime + pc.stime) - (prev->utime + prev->stime);
            cpu = (double)(delta) / total_diff * 100.0;
            prev->utime = pc.utime;
            prev->stime = pc.stime;
        }
        else {
            ProcCpu* val = malloc(sizeof(ProcCpu));
            *val = pc;
            g_hash_table_insert(cpu_table, GINT_TO_POINTER(pid), val);
            cpu = 0.0; // 第一次观察该进程时显示0
        }

        // ---- MEM ----
        double mem = get_proc_mem(pid, mem_total);

        // ---- IO ----
        ProcIO io = { 0 };
        double io_kb = 0.0;
        if (get_proc_io(pid, &io)) {
            ProcIO* prev_io = g_hash_table_lookup(io_table, GINT_TO_POINTER(pid));
            if (prev_io) {
                io_kb = ((io.read_bytes - prev_io->read_bytes) +
                    (io.write_bytes - prev_io->write_bytes)) / 1024.0;
                prev_io->read_bytes = io.read_bytes;
                prev_io->write_bytes = io.write_bytes;
            }
            else {
                ProcIO* val = malloc(sizeof(ProcIO));
                *val = io;
                g_hash_table_insert(io_table, GINT_TO_POINTER(pid), val);
                io_kb = 0.0;
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

    // ---- 恢复之前选中的行 ----
    if (selected_pid != -1&& is_selection==1) 
    {
        GtkTreeIter store_iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &store_iter);
        while (valid) 
        {
            gint pid;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &store_iter, COL_PID, &pid, -1);

            if (pid == selected_pid) 
            {
                GtkTreePath* filter_path = gtk_tree_model_filter_convert_child_path_to_path(GTK_TREE_MODEL_FILTER(filter_model),gtk_tree_model_get_path(GTK_TREE_MODEL(store), &store_iter));
                GtkTreePath* sort_path = gtk_tree_model_sort_convert_child_path_to_path(GTK_TREE_MODEL_SORT(sort_model),filter_path);
                if (sort_path) 
                {
                    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(process_tree_view));
                    gtk_tree_selection_select_path(sel, sort_path);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(process_tree_view), sort_path, NULL, FALSE, 0, 0);
                    gtk_tree_path_free(sort_path);
                }
                gtk_tree_path_free(filter_path);
                break;
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &store_iter);
        }
    }
    prev_cpu = cur_cpu;
    return TRUE;
}

/* ================= 系统状态刷新 ================= */
gboolean update_system_summary(gpointer user_data)
{
    GtkWidget* sys_label = GTK_WIDGET(user_data);

    static CpuTotal prev_cpu = { 0 };
    static DiskTotal prev_disk = { 0 };

    CpuTotal cur_cpu = get_cpu_total();
    DiskTotal cur_disk = get_disk_total();

    double cpu_p = 0.0, disk_kb = 0.0;

    if (prev_cpu.total > 0) {
        long long total_diff = cur_cpu.total - prev_cpu.total;
        long long idle_diff = cur_cpu.idle - prev_cpu.idle;
        if (total_diff > 0)
            cpu_p = 100.0 * (1.0 - (double)idle_diff / total_diff);
    }

    if (prev_disk.sectors > 0) {
        long long sec_diff = cur_disk.sectors - prev_disk.sectors;
        disk_kb = sec_diff * 512.0 / 1024.0;
    }

    prev_cpu = cur_cpu;
    prev_disk = cur_disk;

    double mem_p = get_mem_percent();

    char buf[128];
    snprintf(buf, sizeof(buf),
        "System Total | CPU: %.1f%% | MEM: %.1f%% | Disk: %.1f KB/s",
        cpu_p, mem_p, disk_kb);

    gtk_label_set_text(GTK_LABEL(sys_label), buf);

    return TRUE;
}

gboolean update_system_total(gpointer user_data)
{
    CpuTotal cur_cpu = get_cpu_total();
    DiskTotal cur_disk = get_disk_total();
    static CpuTotal prev_cpu = { 0 };
    static DiskTotal prev_disk = { 0 };

    if (prev_cpu.total > 0) {
        long long total_diff = cur_cpu.total - prev_cpu.total;
        long long idle_diff = cur_cpu.idle - prev_cpu.idle;
        if (total_diff > 0)
            cpu_p = 100.0 * (1.0 - (double)idle_diff / total_diff);
    }

    if (prev_disk.sectors > 0) {
        long long sec_diff = cur_disk.sectors - prev_disk.sectors;
        disk_kb = sec_diff * 512.0 / 1024.0;
    }

    prev_cpu = cur_cpu;
    prev_disk = cur_disk;
    mem_p = get_mem_percent();
    

    /* 更新历史数据 */
    perf_data.cpu[perf_data.index] = cpu_p;
    perf_data.mem[perf_data.index] = mem_p;
    perf_data.disk[perf_data.index] = disk_kb;
    perf_data.index = (perf_data.index + 1) % HISTORY_LEN;

    /* 更新性能面板标签 */
    char buf[64];

    snprintf(buf, sizeof(buf), "CPU %.1f%% %.2f GHz", cpu_p, 1.4);
    gtk_label_set_text(GTK_LABEL(perf_cpu_label), buf);

    snprintf(buf, sizeof(buf), "内存 %.1f%% %.1f GB", mem_p, 16.5);
    gtk_label_set_text(GTK_LABEL(perf_mem_label), buf);

    snprintf(buf, sizeof(buf), "磁盘 %.1f KB/s", disk_kb);
    gtk_label_set_text(GTK_LABEL(perf_disk_label), buf);

    if (cpu_drawing_area)
        gtk_widget_queue_draw(cpu_drawing_area);

    if (mem_drawing_area)
        gtk_widget_queue_draw(mem_drawing_area);

    if (disk_drawing_area)
        gtk_widget_queue_draw(disk_drawing_area);

    return TRUE;
}

gboolean update_cpu_detail_label(gpointer user_data)
{
    CpuInfo info;
    get_cpu_info(&info);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "型号: %s | 核心: %d | 线程: %d | 缓存: %d KB | 当前频率: %.2f GHz | 使用率: %.1f%%",
        info.model, info.cores, info.threads, info.cache_kb, info.freq_ghz,
        cpu_p);

    gtk_label_set_text(GTK_LABEL(cpu_detail_label), buf);
    return TRUE;
}

gboolean update_memory_info(gpointer data)
{
    MemStat m;
    if (!get_mem_stat(&m)) return TRUE;

    // 转 GB（MemInfo 是 KB）
    double used = (m.mem_total - m.mem_free - m.buffers - m.cached) / 1024.0 / 1024.0;
    double avail = (m.mem_free + m.buffers + m.cached) / 1024.0 / 1024.0;
    double cache = (m.buffers + m.cached) / 1024.0 / 1024.0;
    double swap = (m.swap_total - m.swap_free) / 1024.0 / 1024.0;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "已用内存:  %.2f GB\n"
        "可用内存: %.2f GB\n"
        "缓存:      %.2f GB\n"
        "交换空间:  %.2f GB",
        used, avail, cache, swap);

    gtk_label_set_text(GTK_LABEL(mem_info_label), buf);

    return TRUE;
}

gboolean update_disk_info(gpointer user_data)
{
    FILE* fp = fopen("/proc/diskstats", "r");
    if (!fp) return TRUE;

    char line[256];
    DiskStats curr = { 0 };

    while (fgets(line, sizeof(line), fp)) 
    {
        unsigned int major, minor;
        char name[32];
        unsigned long long read_sectors, write_sectors, busy_time;

        sscanf(line, "%u %u %s %*u %*u %llu %*u %*u %*u %llu %*u %*u %llu",
            &major, &minor, name,
            &read_sectors, &write_sectors, &busy_time);

        if (strcmp(name, "sda") == 0) { // sda 或你的磁盘
            curr.read_sectors = read_sectors;
            curr.write_sectors = write_sectors;
            curr.busy_time = busy_time;
            break;
        }
    }
    fclose(fp);

    double delta_read = (curr.read_sectors - last_disk_stats.read_sectors) * 512.0 / 1024.0;
    double delta_write = (curr.write_sectors - last_disk_stats.write_sectors) * 512.0 / 1024.0;
    double delta_busy = (curr.busy_time - last_disk_stats.busy_time) / 10.0; // 百分比

    last_disk_stats = curr;

    char buf[128];
    snprintf(buf, sizeof(buf), "读取速度: %.1f KB/s", delta_read);
    gtk_label_set_text(GTK_LABEL(disk_read_label), buf);

    snprintf(buf, sizeof(buf), "写入速度: %.1f KB/s", delta_write);
    gtk_label_set_text(GTK_LABEL(disk_write_label), buf);

    snprintf(buf, sizeof(buf), "活动时间: %.1f %%", delta_busy);
    gtk_label_set_text(GTK_LABEL(disk_active_label), buf);

    if (disk_drawing_area) gtk_widget_queue_draw(disk_drawing_area);

    return TRUE;
}


//标签创建
GtkWidget* create_cpu_info_label(GtkWidget* parent)
{
    cpu_detail_label = gtk_label_new("正在获取 CPU 信息...");
    gtk_widget_set_halign(cpu_detail_label, GTK_ALIGN_START);
    gtk_widget_set_valign(cpu_detail_label, GTK_ALIGN_START);

    g_timeout_add_seconds(flash_time, update_cpu_detail_label, NULL);

    return cpu_detail_label;
}

//进程面板
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

    g_signal_connect(process_tree_view, "cursor-changed", G_CALLBACK(on_row_selected), NULL);//绑定点击事件

    // 定时刷新
    g_timeout_add_seconds(flash_time, update_process_list, NULL);
    g_timeout_add_seconds(flash_time, update_system_summary, sys_label);

    return process_panel_box;
}

//性能面板
GtkWidget* create_cpu_panel()
{
    /* 最外层：垂直布局 */
    GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(panel, 10);
    gtk_widget_set_margin_end(panel, 10);
    gtk_widget_set_margin_top(panel, 10);
    gtk_widget_set_margin_bottom(panel, 10);

    /* ────────────── 1️⃣ 顶部概要区 ────────────── */
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(panel), header_box, FALSE, FALSE, 0);

    /* CPU 标题 */
    GtkWidget* title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>CPU</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header_box), title, FALSE, FALSE, 0);

    ///* CPU 使用率 + 频率 */
    //perf_cpu_label = gtk_label_new("0%   0.00 GHz");
    //gtk_widget_set_halign(perf_cpu_label, GTK_ALIGN_END);
    //gtk_widget_set_hexpand(perf_cpu_label, TRUE);
    //gtk_box_pack_end(GTK_BOX(header_box),perf_cpu_label, FALSE, FALSE, 0);

    /* 分隔线 */
    gtk_box_pack_start(GTK_BOX(panel),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 5);

    /* ────────────── 2️⃣ 折线图区域 ────────────── */
    cpu_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(cpu_drawing_area, -1, 260);
    gtk_box_pack_start(GTK_BOX(panel),
        cpu_drawing_area, TRUE, TRUE, 0);

    g_signal_connect(cpu_drawing_area, "draw",
        G_CALLBACK(draw_performance),
        GINT_TO_POINTER(PERF_CPU));

    /* 分隔线 */
    gtk_box_pack_start(GTK_BOX(panel),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
        FALSE, FALSE, 5);

    /* ────────────── 3️⃣ 详细信息区 ────────────── */
    GtkWidget* detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(panel), detail_box, FALSE, FALSE, 0);

    GtkWidget* info = create_cpu_info_label(detail_box); gtk_box_pack_start(GTK_BOX(detail_box),info, FALSE, FALSE, 0);

    return panel;
}

GtkWidget* create_memory_panel()
{
    GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(panel, 10);
    gtk_widget_set_margin_end(panel, 10);
    gtk_widget_set_margin_top(panel, 10);
    gtk_widget_set_margin_bottom(panel, 10);

    /* 顶部 */
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(panel), header, FALSE, FALSE, 0);

    GtkWidget* title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='x-large' weight='bold'>内存</span>");
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);

    perf_mem_label = gtk_label_new("0%   0 / 0 GB");
    gtk_widget_set_halign(perf_mem_label, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(header), perf_mem_label, FALSE, FALSE, 0);

    /* 折线图 */
    mem_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(mem_drawing_area, -1, 360);
    gtk_box_pack_start(GTK_BOX(panel),
        mem_drawing_area, TRUE, TRUE, 0);

    g_signal_connect(mem_drawing_area, "draw",
        G_CALLBACK(draw_performance), GINT_TO_POINTER(PERF_MEM));

    /* 详细信息 */
    mem_info_label = gtk_label_new("已用内存:\n可用内存:\n缓存:\n交换空间:");
    gtk_widget_set_halign(mem_info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(panel), mem_info_label, FALSE, FALSE, 0);

    return panel;
}

GtkWidget* create_disk_panel()
{
    GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(panel, 10);
    gtk_widget_set_margin_end(panel, 10);
    gtk_widget_set_margin_top(panel, 10);
    gtk_widget_set_margin_bottom(panel, 10);

    /* 顶部 */
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(panel), header, FALSE, FALSE, 0);

    GtkWidget* title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),"<span size='x-large' weight='bold'>磁盘</span>");
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);

    perf_disk_label = gtk_label_new("0 KB/s");
    gtk_widget_set_halign(perf_disk_label, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(header), perf_disk_label, FALSE, FALSE, 0);

    /* 折线图 */
    disk_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(disk_drawing_area, -1, 360);
    gtk_box_pack_start(GTK_BOX(panel),disk_drawing_area, TRUE, TRUE, 0);

    g_signal_connect(disk_drawing_area, "draw",G_CALLBACK(draw_performance), GINT_TO_POINTER(PERF_DISK));

    /* 详细信息 */
    GtkWidget* info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(panel), info_box, FALSE, FALSE, 0);

    disk_read_label = gtk_label_new("读取速度: 0 KB/s");
    disk_write_label = gtk_label_new("写入速度: 0 KB/s");
    disk_active_label = gtk_label_new("活动时间: 0 %");

    gtk_widget_set_halign(disk_read_label, GTK_ALIGN_START);
    gtk_widget_set_halign(disk_write_label, GTK_ALIGN_START);
    gtk_widget_set_halign(disk_active_label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(info_box), disk_read_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_box), disk_write_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_box), disk_active_label, FALSE, FALSE, 0);

    return panel;
}

GtkWidget* create_performance_panel()
{
    GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    /* ===== 左侧列表 ===== */
    GtkWidget* list = gtk_list_box_new();
    gtk_widget_set_size_request(list, 180, -1);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list),
        GTK_SELECTION_SINGLE);
    gtk_box_pack_start(GTK_BOX(panel), list, FALSE, FALSE, 0);

    const char* items[] = { "CPU", "内存", "磁盘" };
    for (int i = 0; i < 3; i++) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* label = gtk_label_new(items[i]);
        gtk_widget_set_margin_start(label, 10);
        gtk_widget_set_margin_top(label, 8);
        gtk_widget_set_margin_bottom(label, 8);
        gtk_container_add(GTK_CONTAINER(row), label);
        g_object_set_data(G_OBJECT(row), "perf_type",
            GINT_TO_POINTER(i));
        gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
    }

    /* ===== 右侧 Stack ===== */
    perf_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(perf_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_pack_start(GTK_BOX(panel),perf_stack, TRUE, TRUE, 0);

    gtk_stack_add_named(GTK_STACK(perf_stack),
        create_cpu_panel(), "cpu");
    gtk_stack_add_named(GTK_STACK(perf_stack),
        create_memory_panel(), "mem");
    gtk_stack_add_named(GTK_STACK(perf_stack),
        create_disk_panel(), "disk");

    gtk_stack_set_visible_child_name(GTK_STACK(perf_stack), "cpu");
    gtk_list_box_select_row(GTK_LIST_BOX(list),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));

    g_signal_connect(list, "row-selected",
        G_CALLBACK(on_perf_row_selected), NULL);

    return panel;
}

/* ================= 主函数 ================= */
int main(int argc, char* argv[])
{
    gtk_init(&argc, &argv);
    cpu_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    io_table = g_hash_table_new(g_direct_hash, g_direct_equal);

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Linux任务管理器");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 500);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);

    /* ===== 面板选项 ===== */
    GtkWidget* stack = gtk_stack_new();
    GtkWidget* left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(left_box, 150, -1); // 宽度加大到 220
    gtk_box_pack_start(GTK_BOX(hbox), left_box, FALSE, FALSE, 5);

    GtkWidget* btn_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(btn_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(left_box), btn_list, FALSE, FALSE, 0);

    // 创建可点击行，增加字体大小和行高
    GtkWidget* row_process = create_clickable_row("进程", stack, "process");
    GtkWidget* row_performance = create_clickable_row("性能", stack, "performance");

    // 设置行高度
    gtk_widget_set_size_request(row_process, -1, 40);     // 行高 60
    gtk_widget_set_size_request(row_performance, -1, 40); // 行高 60

    // 设置字体更大
    GtkWidget* label_process = gtk_bin_get_child(GTK_BIN(row_process));
    GtkWidget* label_perf = gtk_bin_get_child(GTK_BIN(row_performance));

    PangoAttrList* attrs = pango_attr_list_new();
    PangoAttribute* font_attr = pango_attr_size_new_absolute(28 * PANGO_SCALE); // 28px
    pango_attr_list_insert(attrs, font_attr);
    gtk_label_set_attributes(GTK_LABEL(label_process), attrs);
    gtk_label_set_attributes(GTK_LABEL(label_perf), attrs);
    pango_attr_list_unref(attrs);

    gtk_list_box_insert(GTK_LIST_BOX(btn_list), row_process, -1);
    gtk_list_box_insert(GTK_LIST_BOX(btn_list), row_performance, -1);

    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);

    // 渲染进程面板
    GtkWidget* process_panel = create_process_panel();
    gtk_stack_add_named(GTK_STACK(stack), process_panel, "process");

    // 性能面板
    performance_panel = create_performance_panel();
    gtk_stack_add_named(GTK_STACK(stack), performance_panel, "performance");

    g_timeout_add_seconds(flash_time, update_system_total, NULL);//总状态刷新计时器
    g_timeout_add_seconds(flash_time, update_memory_info, NULL);//总内存刷新计时器
    g_timeout_add_seconds(flash_time, update_disk_info, NULL); // 每秒调用一次刷新函数

    // 默认显示进程面板
    gtk_stack_set_visible_child(GTK_STACK(stack), process_panel);

    gtk_widget_show_all(win);
    gtk_main();

    return 0;
}
