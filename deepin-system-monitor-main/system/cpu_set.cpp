// Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ddlog.h"
#include "cpu_set.h"
#include "private/cpu_set_p.h"

#include "common/common.h"
#include "common/thread_manager.h"
#include "system_monitor_thread.h"
#include "system_monitor.h"
#include "sys_info.h"
extern "C" {
#include "../3rdparty/lscpu.h"
#include "../3rdparty/include/path.h"
#include "../3rdparty/include/xalloc.h"
}

#include <QMap>
#include <QByteArray>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QRegularExpression>
#include <ctype.h>
#include <errno.h>
#include <sched.h>

#define PROC_PATH_STAT "/proc/stat"
#define PROC_PATH_CPUINFO "/proc/cpuinfo"

using namespace common::error;
using namespace common::alloc;
using namespace common::init;
using namespace DDLog;

static bool read_dmi_cache = false;

using namespace DDLog;

namespace core {
namespace system {

/**
   @brief 获取首个有效CPU的当前频率，参考 util-linux 中 lscpu 读取 /proc/cpuinfo 的首个 cpu 的 mhz 信息。
   @note 1. 参考 lsblk_cputype_get_scalmhz 函数，过滤无效cpu
         2. 上游的 lscpu 不再提供 'CPU MHz' 而是 'CPU(s) scaling MHz' ，因为各个核心的频率不定，单纯取平均也存在出入；
            此处的实现参考 util-linux 2.33.1.20-1+dde 版本，取首个有效值，和 lscpu 指令保持一致。
 */
static float first_avaliable_cur_freq(struct lscpu_cxt *cxt, const struct lscpu_cputype *ct)
{
    qCDebug(app) << "Getting first available current frequency";
    if (!cxt) {
        qCWarning(app) << "lscpu_cxt is null";
        return 0.0f;
    }

    size_t i;

    for (i = 0; i < cxt->npossibles; i++) {
        struct lscpu_cpu *cpu = cxt->cpus[i];

        if (!cpu || cpu->type != ct || !is_cpu_present(cxt, cpu))
            continue;
        if (cpu->mhz_max_freq <= 0.0f || cpu->mhz_cur_freq <= 0.0f)
            continue;
        if (cpu->mhz_cur_freq <= 0.0f)
            continue;
        qCDebug(app) << "Found first available frequency:" << cpu->mhz_cur_freq << "at index" << i;
        return cpu->mhz_cur_freq;
    }

    qCDebug(app) << "No available frequency found";
    return 0.0f;
}
// 取所有CPU频率有效值中最大值
static float max_avaliable_cur_freq(struct lscpu_cxt *cxt, const struct lscpu_cputype *ct)
{
    qCDebug(app) << "Getting max available current frequency";
    if (!cxt) {
        qCWarning(app) << "lscpu_cxt is null";
        return 0.0f;
    }

    size_t i;
    float res = 0.0f;

    for (i = 0; i < cxt->npossibles; i++) {
        struct lscpu_cpu *cpu = cxt->cpus[i];

        if (!cpu || cpu->type != ct || !is_cpu_present(cxt, cpu))
            continue;
        if (cpu->mhz_cur_freq <= 0.0f)
            continue;
        res = max(res, cpu->mhz_cur_freq);
    }

    qCDebug(app) << "Max available frequency is" << res;
    return res;
}

// 取所有CPU频率有效值中平均值
static float avg_avaliable_cur_freq(struct lscpu_cxt *cxt, const struct lscpu_cputype *ct)
{
    qCDebug(app) << "Getting average available current frequency";
    if (!cxt) {
        qCWarning(app) << "lscpu_cxt is null";
        return 0.0f;
    }

    size_t i;
    float sum = 0.0f;
    size_t count = 0;

    for (i = 0; i < cxt->npossibles; i++) {
        struct lscpu_cpu *cpu = cxt->cpus[i];

        if (!cpu || cpu->type != ct || !is_cpu_present(cxt, cpu))
            continue;
        if (cpu->mhz_cur_freq <= 0.0f)
            continue;

        sum += cpu->mhz_cur_freq;
        count++;
    }

    if (count == 0) {
        qCDebug(app) << "No valid frequencies found for average calculation";
        return 0.0f;
    }

    float avg = sum / count;
    qCDebug(app) << "Average available frequency is" << avg << "from" << count << "CPUs";
    return avg;
}

static void lscpu_free_context(struct lscpu_cxt *cxt)
{
    size_t i;

    if (!cxt) {
        qCDebug(app) << "lscpu_cxt is null, skipping free";
        return;
    }

    qCDebug(app) << "Freeing lscpu context";
    DBG(MISC, ul_debugobj(cxt, "freeing context"));

    DBG(MISC, ul_debugobj(cxt, " de-initialize paths"));
    ul_unref_path(cxt->syscpu);
    ul_unref_path(cxt->procfs);

    DBG(MISC, ul_debugobj(cxt, " freeing cpus"));
    for (i = 0; i < cxt->npossibles; i++) {
        lscpu_unref_cpu(cxt->cpus[i]);
        cxt->cpus[i] = nullptr;
    }
    DBG(MISC, ul_debugobj(cxt, " freeing types"));
    for (i = 0; i < cxt->ncputypes; i++) {
        lscpu_unref_cputype(cxt->cputypes[i]);
        cxt->cputypes[i] = nullptr;
    }

    free(cxt->present);
    free(cxt->online);
    free(cxt->cputypes);
    free(cxt->cpus);

    for (i = 0; i < cxt->nvuls; i++) {
        free(cxt->vuls[i].name);
        free(cxt->vuls[i].text);
    }
    free(cxt->vuls);

    for (i = 0; i < cxt->nnodes; i++)
        free(cxt->nodemaps[i]);

    free(cxt->nodemaps);
    free(cxt->idx2nodenum);

    lscpu_free_virtualization(cxt->virt);
    lscpu_free_architecture(cxt->arch);

    lscpu_free_caches(cxt->ecaches, cxt->necaches);
    lscpu_free_caches(cxt->caches, cxt->ncaches);

    free(cxt);
    qCDebug(app) << "Freed lscpu context";
}
/*
* borrowed & modified from LINUX bitmap.c
*/

/*
 * Region 9-38:4/10 describes the following bitmap structure:
 * 0       9  12    18          38
 * .........****......****......****......
 *      ^  ^     ^           ^
 *      start  off   group_len         end
 */
//struct region {
//    unsigned int start;
//    unsigned int off;
//    unsigned int group_len;
//    unsigned int end;
//};

//static bool cpuset_set_region(const struct region *r,
//                              cpu_set_t *set, size_t size)
//{
//    unsigned int start;

//    if (r->end >= size)
//        return false;

//    for (start = r->start; start <= r->end; start += r->group_len) {
//        auto len = std::min(r->end - start + 1, r->off);
//        for (size_t pos = 0; pos < len; pos++)
//            CPU_SET_S(start + pos, size, set);
//    }

//    return true;
//}

//static bool cpuset_check_region(const struct region *r)
//{
//    if (r->start > r->end || r->group_len == 0 || r->off > r->group_len)
//        return false;

//    return true;
//}

//static const char *cpuset_getnum(const char *str, unsigned int *num)
//{
//    unsigned long long n;
//    char *ptr;

//    errno = 0;
//    n = strtoull(str, &ptr, 10);
//    if (errno == ERANGE || n != uint(n))
//        return nullptr;

//    *num = uint(n);
//    return ptr;
//}

//static inline bool end_of_str(char c)
//{
//    return c == '\0' || c == '\n';
//}

//static inline bool __end_of_region(char c)
//{
//    return isspace(c) || c == ',';
//}

//static inline bool end_of_region(char c)
//{
//    return __end_of_region(c) || end_of_str(c);
//}

///*
// * The format allows commas and whitespaces at the beginning
// * of the region.
// */
//static const char *cpuset_find_region(const char *str)
//{
//    while (__end_of_region(*str))
//        str++;

//    return end_of_str(*str) ? nullptr : str;
//}

//static const char *cpuset_parse_region(const char *str, struct region *r)
//{
//    str = cpuset_getnum(str, &r->start);
//    if (!str)
//        return nullptr;

//    if (end_of_region(*str))
//        goto no_end;

//    if (*str != '-')
//        return nullptr;

//    str = cpuset_getnum(str + 1, &r->end);
//    if (!str)
//        return nullptr;

//    if (end_of_region(*str))
//        goto no_pattern;

//    if (*str != ':')
//        return nullptr;

//    str = cpuset_getnum(str + 1, &r->off);
//    if (!str)
//        return nullptr;

//    if (*str != '/')
//        return nullptr;

//    return cpuset_getnum(str + 1, &r->group_len);

//no_end:
//    r->end = r->start;
//no_pattern:
//    r->off = r->end + 1;
//    r->group_len = r->end + 1;

//    return end_of_str(*str) ? nullptr : str;
//}

/**
 * cpuset_parselist - convert list format ASCII string to cpuset
 * @buf: read user string from this buffer; must be terminated
 *    with a \0 or \n.
 * @maskp: write resulting mask here
 * @nmaskbits: number of bits in mask to be written
 *
 * Input format is a comma-separated list of decimal numbers and
 * ranges.  Consecutively set bits are shown as two hyphen-separated
 * decimal numbers, the smallest and largest bit numbers set in
 * the range.
 * Optionally each range can be postfixed to denote that only parts of it
 * should be set. The range will divided to groups of specific size.
 * From each group will be used only defined amount of bits.
 * Syntax: range:used_size/group_size
 * Example: 0-1023:2/256 ==> 0,1,256,257,512,513,768,769
 *
 * Returns: 0 on success, -errno on invalid input strings. Error values:
 *
 *   - ``-EINVAL``: wrong region format
 *   - ``-EINVAL``: invalid character in string
 *   - ``-ERANGE``: bit number specified too large for mask
 *   - ``-EOVERFLOW``: integer overflow in the input parameters
 */
//bool cpuset_parse_list(const char *buf, cpu_set_t *set, size_t size)
//{
//    struct region r;
//    CPU_ZERO_S(size, set);

//    while (buf) {
//        buf = cpuset_find_region(buf);
//        if (!buf)
//            return false;

//        buf = cpuset_parse_region(buf, &r);
//        if (!buf)
//            return false;

//        bool ok = cpuset_check_region(&r);
//        if (!ok)
//            return false;

//        ok = cpuset_set_region(&r, set, size);
//        if (!ok)
//            return false;
//    }

//    return true;
//}

CPUSet::CPUSet()
    : d(new CPUSetPrivate())
{
    qCDebug(app) << "CPUSet constructor";
}

CPUSet::CPUSet(const CPUSet &other)
    : d(other.d)
{
    qCDebug(app) << "CPUSet copy constructor";
}

CPUSet &CPUSet::operator=(const CPUSet &rhs)
{
    qCDebug(app) << "CPUSet assignment operator";
    if (this == &rhs)
        return *this;

    d = rhs.d;
    return *this;
}

CPUSet::~CPUSet()
{
    qCDebug(app) << "CPUSet destructor";
    // NOTE: don't delete d here, it's a shared pointer
}

QString CPUSet::modelName() const
{
    qCDebug(app) << "Getting CPU model name";
    if (mIsEmptyModelName == true)
        return d->m_info.value(QStringLiteral("型号名称"));
    else
        return d->m_info.value("Model name");
}

QString CPUSet::vendor() const
{
    qCDebug(app) << "Getting CPU vendor";
    if (mIsEmptyModelName == true)
        return d->m_info.value(QStringLiteral("厂商 ID"));
    else
        return d->m_info.value("Vendor ID");
}

int CPUSet::cpuCount() const
{
    qCDebug(app) << "Getting CPU count";
    return d->m_info.value("CPU(s)").toInt();
}

int CPUSet::socketCount() const
{
    qCDebug(app) << "Getting CPU socket count";
    return d->m_info.value("Socket(s)").toInt();
}

QString CPUSet::virtualization() const
{
    qCDebug(app) << "Getting CPU virtualization";
    return d->m_info.value("Virtualization").isEmpty() ? QObject::tr("Not support") : d->m_info.value("Virtualization");
}

QString CPUSet::curFreq() const
{
    qCDebug(app) << "Getting current CPU frequency";
    if (d->m_info.value("CPU MHz") == "-") {
        qCDebug(app) << "Current CPU frequency not available";
        return "-";
    }
    return common::format::formatHz(d->m_info.value("CPU MHz").toDouble(), common::format::MHz);
}

QString CPUSet::minFreq() const
{
    qCDebug(app) << "Getting min CPU frequency";
    if (mIsEmptyModelName == true)
        return common::format::formatHz(d->m_info.value(QStringLiteral("CPU 最小 MHz")).toDouble(), common::format::MHz);
    else
        return common::format::formatHz(d->m_info.value("CPU min MHz").toDouble(), common::format::MHz);
}

QString CPUSet::maxFreq() const
{
    qCDebug(app) << "Getting max CPU frequency";
    //获取最大CPU频率数值，单位MHz
    qreal MaxFreq = mIsEmptyModelName ? d->m_info.value(QStringLiteral("CPU 最大 MHz")).toDouble() : d->m_info.value("CPU max MHz").toDouble();
    return common::format::formatHz(static_cast<uint>(MaxFreq), common::format::MHz);
}

QString CPUSet::avgFreq() const
{
    qCDebug(app) << "Getting average CPU frequency";
    if (d->m_info.value("CPU avg MHz") == "-") {
        qCDebug(app) << "Average CPU frequency not available";
        return "-";
    }
    return common::format::formatHz(d->m_info.value("CPU avg MHz").toDouble(), common::format::MHz);
}

QString CPUSet::l1dCache() const
{
    qCDebug(app) << "Getting L1d cache";
    auto list = d->m_info.keys();
    QString key = "";
    foreach (QString tmpKey, list) {
        if (tmpKey.contains("L1d")) {
            key = tmpKey;
            qCDebug(app) << "Found L1d cache key:" << key;
            break;
        }
    }
    QString value = d->m_info.value(key);
    qCDebug(app) << "L1d cache value:" << value;
    return value.isNull() ? "-" : value;
}

QString CPUSet::l1iCache() const
{
    qCDebug(app) << "Getting L1i cache";
    auto list = d->m_info.keys();
    QString key = "";
    foreach (QString tmpKey, list) {
        if (tmpKey.contains("L1i")) {
            key = tmpKey;
            qCDebug(app) << "Found L1i cache key:" << key;
            break;
        }
    }
    QString value = d->m_info.value(key);
    qCDebug(app) << "L1i cache value:" << value;
    return value.isNull() ? "-" : value;
}

QString CPUSet::l2Cache() const
{
    qCDebug(app) << "Getting L2 cache";
    auto list = d->m_info.keys();
    QString key = "";
    foreach (QString tmpKey, list) {
        if (tmpKey.contains("L2")) {
            key = tmpKey;
            qCDebug(app) << "Found L2 cache key:" << key;
            break;
        }
    }
    QString value = d->m_info.value(key);
    qCDebug(app) << "L2 cache value:" << value;
    return value.isNull() ? "-" : value;
}

QString CPUSet::l3Cache() const
{
    qCDebug(app) << "Getting L3 cache";
    auto list = d->m_info.keys();
    QString key = "";
    foreach (QString tmpKey, list) {
        if (tmpKey.contains("L3")) {
            key = tmpKey;
            qCDebug(app) << "Found L3 cache key:" << key;
            break;
        }
    }
    QString value = d->m_info.value(key);
    qCDebug(app) << "L3 cache value:" << value;
    return value.isNull() ? "-" : value;
}

QString CPUSet::coreId(int index) const
{
    qCDebug(app) << "Getting core ID for index" << index;
    return d->m_infos.value(index).coreID();
}

const CPUUsage CPUSet::usage() const
{
    qCDebug(app) << "Getting overall CPU usage";
    return d->m_usage;
}

const CPUStat CPUSet::stat() const
{
    qCDebug(app) << "Getting overall CPU stat";
    return d->m_stat;
}

QList<QByteArray> CPUSet::cpuLogicName() const
{
    qCDebug(app) << "Getting CPU logical names";
    return d->m_usageDB.keys();
}

const CPUStat CPUSet::statDB(const QByteArray &cpu) const
{
    qCDebug(app) << "Getting CPU stat for" << cpu;
    return d->m_statDB[cpu];
}

const CPUUsage CPUSet::usageDB(const QByteArray &cpu) const
{
    qCDebug(app) << "Getting CPU usage for" << cpu;
    return d->m_usageDB[cpu];
}

void CPUSet::update()
{
    qCDebug(app) << "Updating CPUSet...";
    read_stats();
    read_overall_info();

    d->cpusageTotal[kLastStat] = d->cpusageTotal[kCurrentStat];
    d->cpusageTotal[kCurrentStat] = d->m_usage->total;
    qCDebug(app) << "CPUSet update finished.";
}

void CPUSet::read_stats()
{
    qCDebug(app) << "Reading CPU stats from" << PROC_PATH_STAT;
    FILE *fp;
    uFile fPtr;
    QByteArray line(BUFSIZ, '\0');
    int ncpu = 0;
    int nr;

    if (!(fp = fopen(PROC_PATH_STAT, "r"))) {
        qCWarning(app) << "Failed to open" << PROC_PATH_STAT << ":" << strerror(errno);
        return;
    }
    fPtr.reset(fp);

    while (fgets(line.data(), BUFSIZ, fp)) {
        if (!strncmp(line.data(), "cpu ", 4)) {
            if (!d->m_stat) {
                // qCDebug(app) << "Allocating overall cpu_stat_t";
                d->m_stat = std::make_shared<struct cpu_stat_t>();
                *d->m_stat = {};
            }

            if (d->m_stat) {
                // all cpu stat in jiffies
                nr = sscanf(line.data() + 5, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &d->m_stat->user,
                            &d->m_stat->nice,
                            &d->m_stat->sys,
                            &d->m_stat->idle,
                            &d->m_stat->iowait,
                            &d->m_stat->hardirq,
                            &d->m_stat->softirq,
                            &d->m_stat->steal,
                            &d->m_stat->guest,
                            &d->m_stat->guest_nice);

                if (nr == 10) {
                    // qCDebug(app) << "Successfully parsed overall CPU stats.";
                    // usage calc
                    QByteArray cpu { "cpu" };
                    d->m_stat->cpu = cpu;

                    if (!d->m_usage) {
                        // qCDebug(app) << "Allocating overall cpu_usage_t";
                        d->m_usage = std::make_shared<struct cpu_usage_t>();
                        *d->m_usage = {};
                    }

                    if (d->m_usage) {
                        d->m_usage->cpu = cpu;
                        d->m_usage->total = d->m_stat->user + d->m_stat->nice + d->m_stat->sys + d->m_stat->idle + d->m_stat->iowait + d->m_stat->hardirq + d->m_stat->softirq + d->m_stat->steal;
                        d->m_usage->idle = d->m_stat->idle + d->m_stat->iowait;
                    }
                } else {
                    qCWarning(app) << "Failed to parse CPU stats from" << PROC_PATH_STAT;
                }
            }
        } else if (!strncmp(line.data(), "cpu", 3)) {
            // per cpu stat in jiffies
            auto stat = std::make_shared<struct cpu_stat_t>();
            if (stat) {
                *stat = {};

                nr = sscanf(line.data() + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &ncpu,
                            &stat->user,
                            &stat->nice,
                            &stat->sys,
                            &stat->idle,
                            &stat->iowait,
                            &stat->hardirq,
                            &stat->softirq,
                            &stat->steal,
                            &stat->guest,
                            &stat->guest_nice);

                if (nr == 11) {
                    QByteArray cpu { "cpu" };
                    cpu.append(QByteArray::number(ncpu));
                    stat->cpu = cpu;
                    // qCDebug(app) << "Successfully parsed stats for" << cpu;

                    // usage calc
                    auto usage = std::make_shared<struct cpu_usage_t>();
                    if (usage) {
                        *usage = {};
                        usage->cpu = cpu;
                        usage->total = stat->user + stat->nice + stat->sys + stat->idle + stat->iowait + stat->hardirq + stat->softirq + stat->steal;
                        usage->idle = stat->idle + stat->iowait;

                        d->m_statDB[stat->cpu] = stat;
                        d->m_usageDB[usage->cpu] = usage;
                    }
                } else {
                    qCWarning(app) << "Failed to parse CPU" << ncpu << "stats from" << PROC_PATH_STAT;
                }
            }
        } else if (!strncmp(line.data(), "btime", 5)) {
            // read boot time in seconds since epoch
            struct timeval btime {};
            long nsec {};
            int nm = sscanf(line.data() + 5, "%ld", &nsec);
            if (nm == 1) {
                btime.tv_sec = nsec;
                btime.tv_usec = 0;
                // qCDebug(app) << "Parsed boot time:" << btime.tv_sec;

                // set sysinfo btime
                auto *monitor = ThreadManager::instance()->thread<SystemMonitorThread>(BaseThread::kSystemMonitorThread)->systemMonitorInstance();
                monitor->sysInfo()->set_btime(btime);
            }   // ::if(nm == 1)
        }   // ::if(btime)
    }   // ::while(fgets)

    if (ferror(fp)) {
        qCWarning(app) << "Error reading" << PROC_PATH_STAT << ":" << strerror(errno);
    }
    qCDebug(app) << "Finished reading CPU stats.";
}

void CPUSet::read_overall_info()
{
    qCDebug(app) << "Reading overall CPU info from /proc/cpuinfo...";
    //proc/cpuinfo
    QList<CPUInfo> infos;
    QProcess process;
    process.start("cat /proc/cpuinfo");
    process.waitForFinished(3000);
    QString cpuinfo = process.readAllStandardOutput();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QStringList processors = cpuinfo.split("\n\n", QString::SkipEmptyParts);
#else
    QStringList processors = cpuinfo.split("\n\n", Qt::SkipEmptyParts);
#endif
    qCDebug(app) << "Found" << processors.count() << "processors in /proc/cpuinfo";

    for (int i = 0; i < processors.count(); ++i) {
        // qCDebug(app) << "Parsing processor" << i;
        CPUInfo info;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QStringList list = processors[i].split("\n", QString::SkipEmptyParts);
#else
        QStringList list = processors[i].split("\n", Qt::SkipEmptyParts);
#endif
        for (QString text : list) {
            if (text.startsWith("processor")) {
                info.setIndex(text.split(":").value(1).toInt());
            } else if (text.startsWith("vendor_id")) {
                info.setVendorId(text.split(":").value(1));
            } else if (text.startsWith("cpu MHz")) {
                info.setCpuFreq(text.split(":").value(1));
            } else if (text.startsWith("core id")) {
                info.setCoreId(text.split(":").value(1).toUInt());
            } else if (text.startsWith("model name")) {
                info.setModelName(text.split(":").value(1));
            } else if (text.startsWith("cache size")) {
                info.setCacheSize(text.split(":").value(1));
            }
        }
        infos.append(info);
    }
    qCDebug(app) << "Finished parsing /proc/cpuinfo";

    //    if (!cpuinfo.contains("model name")) {
    //        mIsEmptyModelName = true;
    //        infos.clear();
    //        // 根据lscpu源码实现获取CPU信息
    //        process.start("lscpu");
    //        process.waitForFinished(3000);
    //        QString lscpu = process.readAllStandardOutput();

    //        QStringList lscpuList = lscpu.split("\n", QString::SkipEmptyParts);
    //        d->m_info.clear();
    //        for (QString lscpuLine : lscpuList) {
    //            QStringList keyValue {};
    //            if (lscpuLine.contains("：")){
    //                keyValue = lscpuLine.split("：", QString::SkipEmptyParts);
    //            } else if (lscpuLine.contains(":")) {
    //                keyValue = lscpuLine.split(":", QString::SkipEmptyParts);
    //            }

    //            if (keyValue.count() > 1)
    //                d->m_info[keyValue.value(0).trimmed()] = keyValue.value(1).trimmed();
    //        }
    //    }
    //    else {
    //        mIsEmptyModelName = false;
    //        read_lscpu();
    //        d->m_infos = infos;
    //    }
    read_lscpu();
    d->m_infos = infos;
    qCDebug(app) << "Finished reading overall CPU info.";
}

void CPUSet::read_dmi_cache_info()
{
    qCDebug(app) << "Reading DMI cache info...";
    if (read_dmi_cache || (d->m_info.contains("L1d cache") && d->m_info.contains("L1i cache") && d->m_info.contains("L2 cache") && d->m_info.contains("L3 cache"))) {
        read_dmi_cache = true;
        qCDebug(app) << "DMI cache info already read or found in lscpu, skipping.";
        return;
    }

    read_dmi_cache = true;
    QProcess process;
    //(specialComType=0)如果不是特殊机型直接返回;
    //(specialComType<=-1)如果是未知类型，则使用之前的判断;
    //(specialComType>=1)如果是hw机型,略过之前的判断
    qCInfo(app) << "common::specialComType value is:" << specialComType;
    if (specialComType == 0) {
        qCDebug(app) << "Skipping DMI cache info for non-special computer type";
        return;
    } else if (specialComType <= -1) {
        qCInfo(app) << "Using dmidecode to check board type";
        process.start("dmidecode", QStringList() << "-s" << "system-product-name");
        process.waitForFinished(-1);
        QString spnInfo = process.readAllStandardOutput();
        if (!spnInfo.contains("KLVV", Qt::CaseInsensitive) && !spnInfo.contains("L540", Qt::CaseInsensitive) && !spnInfo.contains("KLVU", Qt::CaseInsensitive)
            && !spnInfo.contains("PGUV", Qt::CaseInsensitive) && !spnInfo.contains("PGUW", Qt::CaseInsensitive) && !spnInfo.contains("W585", Qt::CaseInsensitive)) {

            process.start("dmidecode");
            process.waitForStarted();
            process.waitForFinished();
            QString result = process.readAll();
            QStringList lines = result.split('\n');
            for (const QString &line : lines) {
                if (line.contains("String 4", Qt::CaseInsensitive)) {
                    result = line;
                    break;
                }
            }
            if (!result.contains("PWC30", Qt::CaseInsensitive)   //w525
                && !result.contains("PGUX", Qt::CaseInsensitive)) {
                process.close();
                qCDebug(app) << "Not a special computer type, skipping DMI cache info";
                return;
            }
        }
    }
    qCInfo(app) << "Processing DMI cache info for special computer type";

    process.start("dmidecode", QStringList() << "-t" << "cache");
    process.waitForFinished(-1);
    QString cacheinfo = process.readAllStandardOutput();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QStringList caches = cacheinfo.split("\n\n", QString::SkipEmptyParts);
#else
    QStringList caches = cacheinfo.split("\n\n", Qt::SkipEmptyParts);
#endif
    process.close();

    qCDebug(app) << "Found" << caches.size() << "cache entries in DMI information";
    QList<QMap<QString, QString>> mapInfos;
    foreach (const QString &item, caches) {
        if (item.isEmpty())
            continue;
        QMap<QString, QString> mapInfo;

        QStringList lines = item.split("\n");
        QString lasKey;
        foreach (const QString &line, lines) {
            if (line.isEmpty())
                continue;

            QStringList words = line.split(": ");
            if (1 == words.size() && words[0].endsWith(":")) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
                lasKey = words[0].replace(QRegExp(":$"), "");
#else
                lasKey = words[0].replace(QRegularExpression(":$"), "");
#endif
                mapInfo.insert(lasKey.trimmed(), " ");
            } else if (1 == words.size() && !lasKey.isEmpty()) {
                mapInfo[lasKey.trimmed()] += words[0];
                mapInfo[lasKey.trimmed()] += "  /  ";
            } else if (2 == words.size()) {
                lasKey = "";
                mapInfo.insert(words[0].trimmed(), words[1].trimmed());
            }
        }
        mapInfos.append(mapInfo);
    }

    for (auto &item : mapInfos) {
        if (!item.contains("Socket Designation") || !item.contains("Maximum Size"))
            continue;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QStringList strList = item["Maximum Size"].split(" ", QString::SkipEmptyParts);
#else
        QStringList strList = item["Maximum Size"].split(" ", Qt::SkipEmptyParts);
#endif
        if (strList.size() != 2)
            continue;

        QString typeStr = strList.last();
        if (typeStr.compare("KB", Qt::CaseInsensitive) == 0)
            typeStr = "KiB";
        else if (typeStr.compare("MB", Qt::CaseInsensitive) == 0) {
            typeStr = "MiB";
        }
        int size = strList.first().toInt();
        if (item["Socket Designation"].contains("L1")) {
            int l1Size = size / 2;
            if (l1Size >= 1024 && typeStr == "KiB") {
                l1Size /= 1024;
                typeStr = "MiB";
            }
            d->m_info.insert("L1d cache", QString::number(l1Size) + " " + typeStr);
            d->m_info.insert("L1i cache", QString::number(l1Size) + " " + typeStr);
        } else {
            if (size >= 1024 && typeStr == "KiB") {
                size /= 1024;
                typeStr = "MiB";
            }
            if (item["Socket Designation"].contains("L2")) {
                d->m_info.insert("L2 cache", QString::number(size) + " " + typeStr);
            } else if (item["Socket Designation"].contains("L3")) {
                d->m_info.insert("L3 cache", QString::number(size) + " " + typeStr);
            }
        }
    }
}

// 获取CPU信息 ut001987
void CPUSet::read_lscpu()
{
    qCDebug(app) << "Reading CPU info using lscpu library...";
    struct lscpu_cxt *cxt;   // CPU信息
    cxt = reinterpret_cast<struct lscpu_cxt *>(xcalloc(1, sizeof(struct lscpu_cxt)));   // 初始化信息
    if (!cxt) {
        qCWarning(app) << "Failed to initialize lscpu context";
        return;
    }
    /* set default cpu display mode if none was specified */
    if (!cxt->show_online && !cxt->show_offline) {
        qCDebug(app) << "Setting default CPU display mode";
        cxt->show_online = 1;
        cxt->show_offline = cxt->mode == LSCPU_OUTPUT_READABLE ? 1 : 0;
    }

    cxt->syscpu = ul_new_path(_PATH_SYS_CPU);
    if (!cxt->syscpu) {
        qCWarning(app) << "Failed to initialize CPUs sysfs handler";
        lscpu_free_context(cxt);
        return;
    }

    if (cxt->prefix)
        ul_path_set_prefix(cxt->syscpu, cxt->prefix);
    cxt->procfs = ul_new_path("/proc");
    if (!cxt->procfs) {
        qCWarning(app) << "Failed to initialize procfs handler";
        lscpu_free_context(cxt);
        return;
    }

    if (cxt->prefix)
        ul_path_set_prefix(cxt->procfs, cxt->prefix);

    qCDebug(app) << "lscpu context initialized. Reading CPU lists and info...";
    lscpu_read_cpulists(cxt);
    lscpu_read_cpuinfo(cxt);
    qCDebug(app) << "Finished reading CPU lists and info.";

    cxt->arch = lscpu_read_architecture(cxt);
    qCDebug(app) << "Read architecture:" << (cxt->arch ? cxt->arch->name : "N/A");
    // 获取CPU的相关信息
    lscpu_read_archext(cxt);
    lscpu_read_vulnerabilities(cxt);
    lscpu_read_numas(cxt);
    lscpu_read_topology(cxt);
    qCDebug(app) << "Read topology and other extensions.";
    lscpu_decode_arm(cxt);
    cxt->virt = lscpu_read_virtualization(cxt);   // 获取CPU的虚拟化信息
    qCDebug(app) << "Read virtualization info:" << (cxt->virt && cxt->virt->hypervisor ? cxt->virt->hypervisor : "N/A");
    struct lscpu_cputype *ct;
    ct = lscpu_cputype_get_default(cxt);   // 获取CPU类型信息
    if (ct) {
        qCDebug(app) << "Default CPU type found. Populating info map.";
        if (cxt->arch)
            d->m_info.insert("Architecture", cxt->arch->name);
        // cpu架构信息
        if (cxt->arch && (cxt->arch->bit32 || cxt->arch->bit64)) {
            QString value;
            if (cxt->arch && (cxt->arch->bit32 || cxt->arch->bit64)) {
                if (cxt->arch->bit32) {
                    value += "32-bit";
                }
                if (cxt->arch->bit64) {
                    value += "64-bit";
                }
                d->m_info.insert("CPU op-mode(s)", value);
            }
        }
        if (ct && ct->addrsz) {
            d->m_info.insert("Address sizes", ct->addrsz);
        }
        // 获取CPU的大小端信息
#if !defined(WORDS_BIGENDIAN)
        d->m_info.insert("Byte Order", "Little Endian");
#else
        d->m_info.insert("Byte Order", "Big Endian");
#endif
        d->m_info.insert("CPU(s)", QString::number(cxt->npresents));
        if (cxt->online) {   // 当前活动CPU信息
            QString key = cxt->hex ? "On-line CPU(s) mask" : "On-line CPU(s) list";
            QString value = "";
            size_t setbuflen = static_cast<size_t>(7 * cxt->maxcpus);
            char setbuf[setbuflen], *p;
            if (cxt->hex) {
                qCDebug(app) << "Creating hex cpumask for online CPUs";
                p = cpumask_create(setbuf, setbuflen, cxt->online, cxt->setsize);
            } else {
                qCDebug(app) << "Creating list for online CPUs";
                p = cpulist_create(setbuf, setbuflen, cxt->online, cxt->setsize);
            }
            d->m_info.insert(key, p);
            qCDebug(app) << key << ":" << p;
        }

        // CPU厂商信息
        if (ct->vendor) {   // CPU厂商信息
            QString strVendor = ct->vendor;
            strVendor = strVendor.simplified();
            strVendor = strVendor.trimmed();
            if (strVendor.isEmpty())
                d->m_info.insert("Vendor ID", "-");
            else
                d->m_info.insert("Vendor ID", ct->vendor);
        } else {
            d->m_info.insert("Vendor ID", "-");
            qCDebug(app) << "Vendor ID not found";
        }

        if (ct && ct->bios_vendor) {   //  BIOS厂商信息
            d->m_info.insert("BIOS Vendor ID", ct->bios_vendor);
        }
        if (ct->modelname) {   // CPU型号名称
            d->m_info.insert("Model name", ct->modelname);
        }
        if (ct->bios_modelname) {   // bios 型号名称
            d->m_info.insert("BIOS Model name", ct->bios_modelname);
        }
        if (ct->bios_family) {   // bios系列
            d->m_info.insert("BIOS CPU family", ct->bios_family);
        }
        if (ct->machinetype) {   //  机器类型
            d->m_info.insert("Machine type", ct->machinetype);
        }
        if (ct->family) {   //  CPU系列
            d->m_info.insert("CPU family", ct->family);
        }
        if (ct->model || ct->revision) {
            d->m_info.insert("Model", ct->revision ? ct->revision : ct->model);
        }
        d->m_info.insert("Thread(s) per core", QString::number(ct->nthreads_per_core));   // 单核线程数
        if (cxt->is_cluster) {
            d->m_info.insert("Core(s) per cluster", QString::number(ct->ncores_per_socket));
        } else {
            d->m_info.insert("Core(s) per socket", QString::number(ct->ncores_per_socket));
        }
        if (ct->nbooks) {
            d->m_info.insert("Socket(s) per book", QString::number(ct->nsockets_per_book));
            if (ct->ndrawers_per_system || ct->ndrawers) {
                d->m_info.insert("Book(s) per drawer", QString::number(ct->nbooks_per_drawer));
                d->m_info.insert("Drawer(s)", QString::number(ct->ndrawers_per_system ? 0 : ct->ndrawers));
            } else {
                d->m_info.insert("Book(s)", QString::number(ct->nbooks_per_drawer ? 0 : ct->nbooks));
            }
        } else {
            if (cxt->is_cluster) {
                if (ct->nr_socket_on_cluster > 0) {
                    d->m_info.insert("Core(s) per cluster", QString::number(ct->nr_socket_on_cluster));
                } else {
                    d->m_info.insert("Socket(s)", "-");
                }
                d->m_info.insert("Cluster(s)", QString::number(ct->nsockets_per_book ? 0 : ct->nsockets));
            } else {
                d->m_info.insert("Socket(s)", QString::number(ct->nsockets_per_book ? 0 : ct->nsockets));
            }
        }
        if (ct->stepping) {
            d->m_info.insert("Stepping", ct->stepping);
        }
        if (ct->freqboost >= 0) {
            d->m_info.insert("Frequency boost", ct->freqboost ? _("enabled") : _("disabled"));
        }

        if (ct->dynamic_mhz) {
            d->m_info.insert("CPU dynamic MHz", ct->dynamic_mhz);
        }
        if (ct->static_mhz) {
            d->m_info.insert("CPU static MHz", ct->static_mhz);
        }
        if (ct->has_freq) {
            float scal = lsblk_cputype_get_scalmhz(cxt, ct);
            QString maxMHz = QString::number(static_cast<double>(lsblk_cputype_get_maxmhz(cxt, ct)), 'f', 4);
            d->m_info.insert("CPU max MHz", maxMHz);
            QString minMHz = QString::number(static_cast<double>(lsblk_cputype_get_minmhz(cxt, ct)), 'f', 4);
            d->m_info.insert("CPU min MHz", minMHz);
            // 取所有CPU频率有效值中最大值
            QString nowMHz = QString::number(static_cast<double>(max_avaliable_cur_freq(cxt, ct)), 'f', 4);
            // 取所有CPU频率有效值中平均值
            QString avgMHz = QString::number(static_cast<double>(avg_avaliable_cur_freq(cxt, ct)), 'f', 4);
            if (scal == 0.0f) {
                nowMHz = "-";
                avgMHz = "-";
            }
            d->m_info.insert("CPU MHz", nowMHz);
            d->m_info.insert("CPU avg MHz", avgMHz);
            qCDebug(app) << "Populated frequency: Min=" << minMHz << "Max=" << maxMHz << "Current=" << nowMHz << "Average=" << avgMHz;
        } else {
            if (CurrentCPUFreq > 0) {
                d->m_info.insert("CPU MHz", QString::number(CurrentCPUFreq));
                d->m_info.insert("CPU max MHz", QString::number(MaxCPUFreq));
            } else {
                d->m_info.insert("CPU MHz", "-");
            }
        }
        if (ct->bogomips) {
            d->m_info.insert("BogoMIPS", ct->bogomips);
        }

        if (ct->physsockets) {
            d->m_info.insert("Physical sockets", QString::number(ct->physsockets));
            d->m_info.insert("Physical chips", QString::number(ct->physchips));
            d->m_info.insert("Physical cores/chip", QString::number(ct->physcoresperchip));
        }
        if (ct->flags) {
            d->m_info.insert("Flags", ct->flags);
        }
    } else {
        qCWarning(app) << "Failed to get CPU type information";
        return;
    }

    // 虚拟化支持
    if (cxt->virt) {
        if (cxt->virt->cpuflag && !strcmp(cxt->virt->cpuflag, "svm")) {
            d->m_info.insert("Virtualization", "AMD-V");
        } else if (cxt->virt->cpuflag && !strcmp(cxt->virt->cpuflag, "vmx")) {
            d->m_info.insert("Virtualization", "VT-x");
        }
        if (cxt->virt->hypervisor) {
            d->m_info.insert("Hypervisor", "cxt->virt->hypervisor");
        }
    }
    qCDebug(app) << "Populating cache information...";
    /* Section: caches */
    if (cxt->ncaches) {
        const char *last = nullptr;
        for (size_t i = 0; i < cxt->ncaches; i++) {
            const char *name = cxt->caches[i].name;
            uint64_t sz;
            int n = 0;

            if (last && strcmp(last, name) == 0) {
                continue;
            }
            sz = lscpu_get_cache_full_size(cxt, name, &n);
            if (!sz)
                continue;
            if (!cxt->bytes) {
                char *tmp = size_to_human_string(   // 获取当前cache的值
                        SIZE_SUFFIX_3LETTER | SIZE_SUFFIX_SPACE,
                        sz);
                QString value = QString(tmp);   //.replace("MiB","MB");
                if (tmp) {
                    free(tmp);
                    tmp = nullptr;
                }
                // value = QString(value).replace("KiB","KB");
                //  value = QString(value).replace("GiB","GB");
                d->m_info.insert(QString(name) + " cache", value);
            }
            last = name;
        }
    }
    read_dmi_cache_info();
    // 某些CPU不带有缓存用‘-’替代
    if (!d->m_info.contains("L1d cache")) {
        d->m_info.insert("L1d cache", "-");
    }
    if (!d->m_info.contains("L1i cache")) {
        d->m_info.insert("L1i cache", "-");
    }
    if (!d->m_info.contains("L2 cache")) {
        d->m_info.insert("L2 cache", "-");
    }
    if (!d->m_info.contains("L3 cache")) {
        d->m_info.insert("L3 cache", "-");
    }
    for (size_t i = 0; i < cxt->necaches; i++) {
        struct lscpu_cache *ca = &cxt->ecaches[i];
        if (ca->size == 0)
            continue;
        if (!cxt->bytes) {
            char *tmp = size_to_human_string(
                    SIZE_SUFFIX_3LETTER | SIZE_SUFFIX_SPACE,
                    ca->size);
            d->m_info.insert(ca->name, tmp);
            if (tmp) {
                free(tmp);
                tmp = nullptr;
            }
        }
    }
    lscpu_free_context(cxt);
    qCDebug(app) << "Finished reading CPU info using lscpu library.";
}

qulonglong CPUSet::getUsageTotalDelta() const
{
    if (d->cpusageTotal[kCurrentStat] <= d->cpusageTotal[kLastStat])
        return 1;

    return d->cpusageTotal[kCurrentStat] - d->cpusageTotal[kLastStat];
}

}   // namespace system
}   // namespace core
