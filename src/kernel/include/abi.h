/* =============================================================================
 * include/abi.h - 鐢ㄦ埛鎬佺郴缁熻皟鐢?ABI 鐨勫崟涓€鏉ユ簮锛坴1锛? *
 * 杩欎釜澶存枃浠跺悓鏃惰鍐呮牳鍜岀敤鎴峰簱锛坰rc/user/ulib.h锛夊寘鍚紝鍙斁璺ㄨ竟鐣岀殑濂戠害锛? *   - legacy int 0x81 璋冪敤鍙凤紙SYS_*锛夛紱int 0x80 鐨?Native ABI 缂栧彿鍦? *     proc/trap/native.h锛圢ATIVE_SYS_*锛夛紝涓ゅ闂ㄧ紪鍙风┖闂翠簰涓嶇浉骞诧紱
 *   - ABI 鐗堟湰銆侀瓟鏁般€乫eature bits锛? *   - 鐢ㄦ埛鍙鍏变韩缁撴瀯浣擄紝甯冨眬鐢辨湰鏂囦欢鏈熬鐨?_Static_assert 鍥哄畾锛? *   - legacy 闂ㄥ彲鑳借繑鍥炵殑鐢ㄦ埛鍙閿欒鐮侊紙澶嶇敤 core/error.h 鐨?E_* 瀛愰泦锛夈€? *
 * 鐗堟湰瑙勫垯锛坴1 鍐荤粨锛岃瑙?docs/api.md / docs/native-abi.md 搂18锛夛細
 *   1. 宸叉湁缂栧彿銆佺粨鏋勪綋瀛楁鍋忕Щ/澶у皬銆侀敊璇爜鍊间笉寰楁敼鍙橈紱
 *   2. 鏂板璋冪敤鍙兘杩藉姞缂栧彿锛?= SYS_NR_MAX + 1锛夛紝骞跺悓姝?bump
 *      OS_ABI_VERSION_MINOR锛? *   3. 缁撴瀯浣撳彧鑳藉熬閮ㄨ拷鍔犲瓧娈碉紝涓斿繀椤诲悓姝ユ洿鏂颁笅闈㈢殑 _Static_assert锛? *   4. 鍒犻櫎鐨勮皟鐢ㄤ繚鐣欑紪鍙凤紝legacy 闂ㄨ繑鍥?-1锛圗_INVAL 璇箟锛夈€? * ========================================================================== */
#ifndef OS_INCLUDE_ABI_H
#define OS_INCLUDE_ABI_H

#include "types.h"
#include "../core/error.h"   /* E_*锛歭egacy 闂?鏂囦欢绯荤粺鍙兘鍘熸牱杩斿洖鐨勯敊璇爜 */

/* =============================================================================
 * 1. ABI 鐗堟湰涓庢爣璇? * ========================================================================== */
#define OS_ABI_MAGIC          0x4F534142u   /* "OSAB"锛堝皬绔瓧鑺傚簭 'O','S','A','B'锛?*/
#define OS_ABI_VERSION        1u            /* 澶存枃浠?缂栧彿绌洪棿鐨?major 鐗堟湰 */
#define OS_ABI_VERSION_MAJOR  1
#define OS_ABI_VERSION_MINOR  4

/* SYS_ABI_VERSION 杩斿洖鍊肩殑浣?16 浣嶏紙feature bits锛夛細鍙涓嶅噺銆? *
 * 娉ㄦ剰锛歜it2-4 鍙〃绀哄唴鏍稿唴閮ㄥ瓨鍦ㄧ増鏈竴鑷存€ф鏌ワ紙internal only锛夛紝
 * 涓嶆槸瀵瑰椹卞姩/鏈嶅姟 API锛屼笉鎵胯鍏煎锛屼篃涓嶄唬琛ㄨ鎺ㄨ繘椹卞姩鏈嶅姟鍖栥€?*/
#define OS_ABI_F_NATIVE_GATE      (1u << 0)  /* int 0x80 Native ABI 闂ㄥ凡瀹夎 */
#define OS_ABI_F_VERSIONED_OPS    (1u << 1)  /* 鍐呮牳鍐呴儴鐗堟湰鏍￠獙瀛樺湪锛坕nternal only锛?*/
#define OS_ABI_F_DEVICE_API_V1    (1u << 2)  /* internal only锛歞ev/device.h 鍐呴儴 ops 鐗堟湰 */
#define OS_ABI_F_VFS_BACKEND_V1   (1u << 3)  /* internal only锛歏FS 鍚庣娉ㄥ唽琛ㄦ牎楠?*/
#define OS_ABI_F_SERVICE_MANIFEST_V1 (1u << 4) /* internal only锛氭湇鍔℃敞鍐屽叆鍙ｆ牎楠?*/

#define OS_ABI_FEATURES                                                     \
    (OS_ABI_F_NATIVE_GATE | OS_ABI_F_VERSIONED_OPS |                        \
     OS_ABI_F_DEVICE_API_V1 | OS_ABI_F_VFS_BACKEND_V1 |                     \
     OS_ABI_F_SERVICE_MANIFEST_V1)

/* =============================================================================
 * 2. legacy int 0x81 绯荤粺璋冪敤鍙凤紙1..58 涓烘棦鏈夎皟鐢紝鍙縼绉讳笉鏀硅涔夛級
 *
 * 鍙傛暟绾﹀畾锛歟bx/ecx/edx 渚濇涓虹 1/2/3 涓弬鏁帮紱杩斿洖鍊间负 eax銆? * >= 0 鎴愬姛锛? 0 澶辫触锛坙egacy 闂ㄥぇ澶氭暟澶辫触杩斿洖 -1锛夈€? * ========================================================================== */
#define SYS_WRITE   1   /* ebx=buf, ecx=len            -> 鍐欏叆瀛楄妭鏁?*/
#define SYS_GETPID  2   /*                              -> 褰撳墠 pid */
#define SYS_UPTIME  3   /*                              -> 杩愯绉掓暟 */
#define SYS_YIELD   4   /*                              -> 0 */
#define SYS_EXIT    5   /* ebx=閫€鍑虹爜                   -> 涓嶈繑鍥?*/
#define SYS_OPEN    6   /* ebx=璺緞, ecx=鏍囧織           -> 鏂囦欢鍙ユ焺 */
#define SYS_FREAD   7   /* ebx=鍙ユ焺, ecx=buf, edx=len   -> 璇诲埌瀛楄妭鏁?*/
#define SYS_FWRITE  8   /* ebx=鍙ユ焺, ecx=buf, edx=len   -> 鍐欏叆瀛楄妭鏁?*/
#define SYS_CLOSE   9   /* ebx=鍙ユ焺                     -> 0 */
#define SYS_GETCHAR 10  /*                              -> 瀛楃鎴?-1 */
#define SYS_LIST    11  /* ebx=璺緞, ecx=搴忓彿, edx=buf     -> 0 / -1 */
#define SYS_DRIVES  12  /* ebx=搴忓彿, ecx=buf, edx=max      -> 0 / -1 */
#define SYS_MKDIR   13  /* ebx=璺緞                       -> 0 / -1 */
#define SYS_UNLINK  14  /* ebx=璺緞                       -> 0 / -1 */
#define SYS_STAT    15  /* ebx=璺緞, ecx=buf(sys_stat_t)  -> 0 / -1 */
#define SYS_CHDIR   16  /* ebx=璺緞                       -> 0 / -1 */
#define SYS_GETCWD  17  /* ebx=buf, ecx=max               -> 闀垮害 / -1 */
#define SYS_FORK    18  /*                                -> 瀛?pid / 0锛堝瓙杩涚▼锛?*/
#define SYS_EXEC    19  /* ebx=璺緞                       -> 涓嶈繑鍥烇紙鎴愬姛锛?*/
#define SYS_WAIT    20  /* ebx=pid, ecx=&status           -> 瀛?pid / -1 */
#define SYS_INPUT_SET 21 /* ebx=buf, ecx=len              -> 0 / -1锛堢敾鍒拌緭鍏ユ锛?*/
#define SYS_BRK     22  /* ebx=澧為噺                       -> 鏂扮殑鍫嗛《 / -1 */
#define SYS_KILL    23  /* ebx=pid, ecx=sig                -> 0 / -1 */
#define SYS_SLEEP   24  /* ebx=姣                        -> 0 */
#define SYS_PIPE    25  /* ebx=鐢ㄦ埛 int[2]                 -> 0 / -1 */
#define SYS_DUP2    26  /* ebx=鏃?fd, ecx=鏂?fd            -> 鏂?fd / -1 */
#define SYS_REBOOT  27  /*                                -> 涓嶈繑鍥烇紙鏈哄櫒澶嶄綅锛?*/
#define SYS_PANIC   28  /*                                -> 涓嶈繑鍥烇紙璋冭瘯鐢細瑙﹀彂鍐呮牳 panic锛?*/
#define SYS_OOPS    29  /*                                -> 涓嶈繑鍥烇紙璋冭瘯鐢細瑙﹀彂鍐呮牳 oops锛?*/
#define SYS_SIGACTION  30 /* ebx=sig, ecx=handler           -> 鏃?handler / -1 */
#define SYS_SIGRETURN  31 /*                                -> 鎭㈠淇″彿鍓嶇殑鐜板満 */
#define SYS_SYNC       32 /*                                -> 0 / -1锛堣惤鐩樺埌鎸佷箙鍖栬澶囷級 */
#define SYS_TIME       33 /* ebx=缂撳啿, ecx=闀垮害             -> 0锛堟牸寮忓寲鏃堕棿瀛楃涓诧級 */
#define SYS_FSCK       34 /* ebx=缂撳啿, ecx=闀垮害             -> 閿欒鏁帮紙缂撳啿閲屾槸鎽樿锛?*/
#define SYS_PCI        35 /* ebx=index, ecx=缂撳啿            -> 0锛圥CI 璁惧淇℃伅锛? -1 */
#define SYS_POWEROFF   36 /*                                -> 涓嶈繑鍥烇紙ACPI S5 鍏虫満锛?*/
#define SYS_MOUSE      37 /* ebx=鐢ㄦ埛 int[3]                -> 0锛坽x,y,buttons}锛?*/
#define SYS_HANG       38 /* 璋冭瘯锛氬叧涓柇姝诲惊鐜紙楠岃瘉 NMI 鐪嬮棬鐙楋級 */
#define SYS_ETH        39 /* ebx=0 鍙栦俊鎭?/ 1 鍙戞祴璇曞抚, ecx=缂撳啿 */
#define SYS_RESINFO    40 /* ebx=鐢ㄦ埛 u32[10]               -> 0锛堣祫婧愮粺璁★級 */
#define SYS_GETUID     41 /*                                -> uid */
#define SYS_GETGID     42 /*                                -> gid */
#define SYS_CHMOD      43 /* ebx=璺緞, ecx=mode             -> 0 / -1 */
#define SYS_CHOWN      44 /* ebx=璺緞, ecx=uid, edx=gid     -> 0 / -1锛坮oot锛?*/
#define SYS_SETUID     45 /* ebx=uid, ecx=gid               -> 0 / -1锛坮oot 鎵嶈兘闄嶆潈锛?*/
#define SYS_CAP_DROP   46 /* ebx=fd, ecx=瑕佹挙閿€鐨?rights    -> 0 / -1 */
#define SYS_CAP_SEND   47 /* ebx=pid, ecx=fd, edx=rights   -> 鐩爣 fd / -1 */
#define SYS_MMAP       48 /* ebx=len, ecx=prot             -> 鐢ㄦ埛 VA / -1 */
#define SYS_MUNMAP     49 /* ebx=addr, ecx=len             -> 0 / -1 */
#define SYS_UDP_SEND   50 /* ebx=&udp_req                 -> 0 / -1 */
#define SYS_UDP_RECV   51 /* ebx=&udp_req                 -> payload len / -1 */
#define SYS_SOCKET     52 /* ebx=domain, ecx=type, edx=proto -> fd / -1 */
#define SYS_SENDTO     53 /* ebx=fd, ecx=&udp_req          -> 0 / -1 */
#define SYS_RECVFROM   54 /* ebx=fd, ecx=&udp_req          -> len / -1 */
#define SYS_CHROOT     55 /* ebx=path                      -> 0 / -1 */
#define SYS_TCP_TEST   56 /* 杩愯涓€娆?TCP echo 鑷           -> 0 / -1 */
#define SYS_BLK_READ   57 /* ebx=lba, ecx=user buf           -> 0 / -1 */
#define SYS_RENAME     58 /* ebx=old path, ecx=new path      -> 0 / -1锛堝悓鍗?rename锛?*/

/* v1 鏂板锛堝彧杩藉姞锛夛細鏍囬噺 ABI 鐗堟湰鏌ヨ銆? * 杩斿洖 (OS_ABI_VERSION << 16) | OS_ABI_FEATURES锛涢珮 16 浣嶆槸 major锛? * 浣?16 浣嶆槸 feature bits銆傞渶瑕佸畬鏁寸粨鏋勶紙major/minor/patch/features锛夋椂
 * 璧?int 0x80 鐨?NATIVE_SYS_ABI_VERSION锛?x00锛夈€?*/
#define SYS_ABI_VERSION 59

/* v1.1 鏂板锛堝彧杩藉姞锛夛細杩涚▼/鍐呭瓨琛ョ己锛坧m 浼氳瘽锛夈€? * SYS_WAIT(20) 鐨勯樆濉炶涔変繚鎸佷笉鍙橈紱甯?options 鐨勭瓑寰呰蛋 SYS_WAITPID銆?*/
#define SYS_WAITPID    60 /* ebx=pid, ecx=&status, edx=options -> 瀛?pid / 0 / -1 */
#define SYS_GETPPID    61 /*                                    -> 鐖?pid锛?=鏃狅級 */
#define SYS_MPROTECT   62 /* ebx=addr, ecx=len, edx=prot         -> 0 / -1 */

/* v1.2 鏂板锛堝彧杩藉姞锛夛細杈撳叆妗嗗厜鏍囨樉绀?闅愯棌銆? * 鍙湪 framebuffer 鐨勮緭鍏ユ閲岀敾涓€鏉′笅鍒掔嚎锛屼笉鍐欎覆鍙ｃ€佷笉鍔ㄨ緭鍑哄尯锛? * 浣嶇疆璺熼殢鏈€鍚庝竴娆?SYS_INPUT_SET 鐢诲嚭鐨勮灏俱€?*/
#define SYS_CONSOLE_CURSOR 63 /* ebx=0 闅愯棌 / 1 鏄剧ず              -> 0 / -1 */

/* v1.3 鏂板锛堝彧杩藉姞锛夛細Linux 寮忚处鍙风郴缁燂紙P1 韬唤锛夈€? *
 * 鍑嵁妯″瀷涓?POSIX 涓€鑷达細ruid/euid/suid + rgid/egid/sgid + 闄勫姞缁勩€? * 宸叉湁鐨?SYS_GETUID/SYS_GETGID 杩斿洖鐪熷疄 uid/gid锛孲YS_GETEUID/SYS_GETEGID
 * 杩斿洖鐢熸晥韬唤锛涙潈闄愭鏌ヤ竴寰嬬敤鐢熸晥韬唤锛坋uid锛夛紝root锛坋uid==0锛変繚鐣? * CAP_DAC_OVERRIDE 璇箟銆? *
 * SYS_LOGIN 鏄?PAM-lite"鍐呮牳閽╁瓙锛氬彛浠ゅ湪鏍稿唴鏍￠獙锛坰ha256 + 鎭掑畾鏃堕棿姣旇緝锛夛紝
 * 鐢ㄦ埛鎬?login/getty 鍙礋璐ｆ彁绀恒€佷細璇濆墠绔笌 delay銆俢md 瑙佷笅闈㈢殑 LOGIN_CMD_*銆?*/
#define SYS_SETEUID    64 /* ebx=euid                              -> 0 / -1 */
#define SYS_SETEGID    65 /* ebx=egid                              -> 0 / -1 */
#define SYS_SETGID     66 /* ebx=gid                               -> 0 / -1锛坮oot锛?*/
#define SYS_GETEUID    67 /*                                       -> euid */
#define SYS_GETEGID    68 /*                                       -> egid */
#define SYS_GETGROUPS  69 /* ebx=buf, ecx=max                      -> 缁勬暟 / -1 */
#define SYS_SETGROUPS  70 /* ebx=buf, ecx=n                        -> 0 / -1锛坮oot锛?*/
#define SYS_SETUMASK   71 /* ebx=mask                              -> 鏃?mask */
#define SYS_LOGIN      72 /* ebx=cmd, ecx=a, edx=b                 -> 瑙佷笅 */

/* SYS_LOGIN 鐨?cmd锛坋cx/edx 鐨勫惈涔夐殢 cmd 鍙樺寲锛?*/
#define LOGIN_CHECK    0  /* -> 1 = 鏈?tty 闇€瑕佺櫥褰曪紱0 = 宸茬櫥褰?鑷姩鐧诲綍 */
#define LOGIN_AUTH     1  /* ecx=鐢ㄦ埛鍚? edx=鍙ｄ护 -> 0 鎴愬姛 / -1 澶辫触 / -2 閿佸畾 */
#define LOGIN_WHO      2  /* ecx=buf, edx=max -> 浼氳瘽鏁帮紙鍐?"user tty pid" 琛岋級 */
#define LOGIN_PASSWD   3  /* ecx=鏃у彛浠? edx=鏂板彛浠?-> 0 / -1 */
#define LOGIN_LOGOUT   4  /* -> 0锛堢粨鏉熷綋鍓嶄細璇濓級 */
#define LOGIN_LAST     5  /* ecx=buf, edx=max -> 瀹¤琛屾暟 */
#define LOGIN_SU       6  /* ecx=鐢ㄦ埛鍚? edx=鍙ｄ护 -> 0 / -1 / -2 */
#define LOGIN_SUDO     7  /* ecx=鍙ｄ护 -> 0 / -1锛坵heel 缁勶級 */
#define LOGIN_SUDO_END 8  /* -> 0锛坋uid 鍥炲埌 ruid锛?*/
#define LOGIN_INFO     9  /* ecx=buf, edx=max -> "uid=.. euid=.. gid=.. name=.." */
#define LOGIN_PASSWD_USER 10 /* ecx=鐢ㄦ埛鍚? edx=鏂板彛浠?-> 0 / -1锛坮oot 缁欎粬浜烘敼锛?*/
#define LOGIN_USERADD  11 /* ecx=鐢ㄦ埛鍚? edx=uid(0=鑷姩) -> 0 / -1锛坮oot锛?*/
#define LOGIN_USERDEL  12 /* ecx=鐢ㄦ埛鍚?-> 0 / -1锛坮oot锛?*/
#define LOGIN_USERMOD  13 /* ecx=鐢ㄦ埛鍚? edx=gid -> 0 / -1锛坮oot 鎶婄敤鎴峰姞杩涚粍锛?*/

/* 闄勫姞缁勪笂闄愶紙proc_t.groups[]銆丼YS_GETGROUPS/SETGROUPS 缂撳啿锛?*/
#define ABI_NGROUPS_MAX 16

/* SYS_WAITPID 鐨?options 浣?*/
#define WNOHANG 1

/* SYS_MPROTECT 鐨?prot 浣嶏紙鏁板€兼部鐢?POSIX锛?*/
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* 鐢ㄦ埛鍙淇″彿缂栧彿锛堝綋鍓嶅彧鏆撮湶 SIGCHLD锛涙暟鍊间笌 proc/proc.h 涓€鑷达級 */
#define SIGCHLD 17
#define SIG_DFL 0u
#define SIG_IGN 1u

/* 宸插垎閰嶇殑鏈€澶х紪鍙凤紱鏂板璋冪敤蹇呴』浠?SYS_NR_MAX + 1 寮€濮?*/
#define SYS_LINK      73 /* ebx=old path, ecx=new path      -> 0 / -1锛堢‖閾炬帴锛?*/
#define SYS_SYMLINK   74 /* ebx=target, ecx=link path      -> 0 / -1锛堢鍙烽摼鎺ワ級 */
#define SYS_READLINK  75 /* ebx=path, ecx=buf, edx=max     -> 鐩爣闀垮害 / -1 */
#define SYS_LSTAT     76 /* ebx=path, ecx=&stat[6]         -> 0 / -1锛堜笉璺熼殢鏈锛?*/
#define SYS_PKG       77 /* ebx=op, ecx=a, edx=b           -> 0 / -1（组件操作） */
#define SYS_LISTEN    78 /* ebx=port, ecx=backlog          -> 0 / -1 */
#define SYS_ACCEPT    79 /* ebx=sock, ecx=&addr            -> 0 / -1 */

#define SYS_NR_MAX 79

/* =============================================================================
 * 3. 鐢ㄦ埛鍙鍏变韩缁撴瀯浣擄紙甯冨眬鍐荤粨锛岃鏂囦欢鏈熬鏂█锛? * ========================================================================== */

/* SYS_STAT 鍐欏嚭锛? 涓?u32锛屼笌鍐呮牳 fs_stat_t 鐨勫瓧娈甸『搴忎竴鑷?*/
typedef struct {
    uint32_t type;      /* 0=none, 1=file, 2=dir */
    uint32_t size;
    uint32_t mtime;     /* epoch 绉?*/
    uint32_t mode;      /* 浣?9 浣?rwxrwxrwx */
    uint32_t uid;
    uint32_t gid;
} sys_stat_t;

/* SYS_UDP_SEND / SYS_UDP_RECV / SYS_SENDTO / SYS_RECVFROM 鐨勮姹?搴旂瓟缂撳啿銆? * 鐢ㄦ埛鍏堝～ ip/port/len/data锛孲YS_UDP_RECV / SYS_RECVFROM 杩斿洖鏃跺唴鏍稿洖濉? * ip/port/len锛坉ata 閲屾槸 payload锛夈€俵en 涓嶈兘瓒呰繃 sizeof(data)=64銆?*/
struct udp_req {
    uint8_t  ip[4];
    uint16_t port;
    uint16_t len;
    uint8_t  data[64];
};

/* SYS_MOUSE 鍐欏嚭锛? 涓?int32锛寋x, y, buttons} */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} sys_mouse_t;

/* SYS_RESINFO 鍐欏嚭锛?0 涓?u32锛岄『搴忓浐瀹?*/
typedef struct {
    uint32_t free_pages;    /* pmm 绌洪棽椤?*/
    uint32_t total_pages;   /* pmm 鎬婚〉 */
    uint32_t heap_used;     /* 鍐呮牳鍫嗗凡鐢ㄥ瓧鑺?*/
    uint32_t heap_total;    /* 鍐呮牳鍫嗘€诲瓧鑺?*/
    uint32_t proc_count;    /* 杩涚▼鏁?*/
    uint32_t syscall_count; /* legacy 闂ㄨ皟鐢ㄦ鏁?*/
    uint32_t cow_copies;    /* COW 澶嶅埗娆℃暟 */
    uint32_t fs_nodes;      /* 鏂囦欢绯荤粺鑺傜偣鏁?*/
    uint32_t devices;       /* 璁惧鏁?*/
    uint32_t pci_devices;   /* PCI 璁惧鏁?*/
} sys_resinfo_t;

/* =============================================================================
 * 4. 鐢ㄦ埛鍙閿欒鐮侊紙core/error.h 鐨?E_* 瀛愰泦锛? *
 * legacy 闂ㄦ病鏈夌粺涓€ errno锛氬鏁?syscall 澶辫触杩斿洖 -1锛屼絾鏂囦欢绯荤粺璺緞
 * 锛堝 fs_sync/fs_write 鐨勯敊璇級鍙兘鎶婁笅闈㈢殑鍊煎師鏍峰甫鍒扮敤鎴锋€併€? * Native ABI 鏈夊彟涓€濂?NATIVE_E_*锛坧roc/trap/native.h锛夛紝鏁板€间笉鍚岋紝涓嶈兘娣风敤銆? * ========================================================================== */
_Static_assert(E_OK == 0, "ABI v1: E_OK must stay 0");
_Static_assert(E_INVAL == -1, "ABI v1: E_INVAL must stay -1");
_Static_assert(E_NOENT == -2, "ABI v1: E_NOENT must stay -2");
_Static_assert(E_NOSPC == -3, "ABI v1: E_NOSPC must stay -3");
_Static_assert(E_TYPE == -4, "ABI v1: E_TYPE must stay -4");
_Static_assert(E_BADH == -5, "ABI v1: E_BADH must stay -5");
_Static_assert(E_EXIST == -6, "ABI v1: E_EXIST must stay -6");
_Static_assert(E_FULL == -7, "ABI v1: E_FULL must stay -7");
_Static_assert(E_ACCES == -8, "ABI v1: E_ACCES must stay -8");
_Static_assert(E_BUSY == -9, "ABI v1: E_BUSY must stay -9");

/* =============================================================================
 * 5. 缂栬瘧鏈熷竷灞€鏂█锛?Werror 涓嬪悓鏍峰繀椤婚€氳繃锛? * ========================================================================== */
_Static_assert(OS_ABI_MAGIC == 0x4F534142u, "ABI v1: magic is frozen");

_Static_assert(sizeof(sys_stat_t) == 24, "ABI v1: sys_stat_t is 6 x u32");
_Static_assert(__builtin_offsetof(sys_stat_t, type) == 0,
               "ABI v1: sys_stat_t.type offset");
_Static_assert(__builtin_offsetof(sys_stat_t, size) == 4,
               "ABI v1: sys_stat_t.size offset");
_Static_assert(__builtin_offsetof(sys_stat_t, mtime) == 8,
               "ABI v1: sys_stat_t.mtime offset");
_Static_assert(__builtin_offsetof(sys_stat_t, mode) == 12,
               "ABI v1: sys_stat_t.mode offset");
_Static_assert(__builtin_offsetof(sys_stat_t, uid) == 16,
               "ABI v1: sys_stat_t.uid offset");
_Static_assert(__builtin_offsetof(sys_stat_t, gid) == 20,
               "ABI v1: sys_stat_t.gid offset");

_Static_assert(sizeof(struct udp_req) == 72, "ABI v1: udp_req is 72 bytes");
_Static_assert(__builtin_offsetof(struct udp_req, ip) == 0,
               "ABI v1: udp_req.ip offset");
_Static_assert(__builtin_offsetof(struct udp_req, port) == 4,
               "ABI v1: udp_req.port offset");
_Static_assert(__builtin_offsetof(struct udp_req, len) == 6,
               "ABI v1: udp_req.len offset");
_Static_assert(__builtin_offsetof(struct udp_req, data) == 8,
               "ABI v1: udp_req.data offset");

_Static_assert(sizeof(sys_mouse_t) == 12, "ABI v1: sys_mouse_t is 3 x s32");

_Static_assert(sizeof(sys_resinfo_t) == 40, "ABI v1: sys_resinfo_t is 10 x u32");
_Static_assert(__builtin_offsetof(sys_resinfo_t, pci_devices) == 36,
               "ABI v1: sys_resinfo_t.pci_devices offset");

#endif /* OS_INCLUDE_ABI_H */
