package tv.danmaku.ijk.media.player;

import android.annotation.TargetApi;
import android.media.MediaCodecInfo;
import android.media.MediaCodecInfo.CodecCapabilities;
import android.media.MediaCodecInfo.CodecProfileLevel;
import android.os.Build;
import android.text.TextUtils;
import android.util.Log;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * 对系统 {@link MediaCodecInfo} 的包装类，用于对候选硬件解码器进行评分排序。
 *
 * <p>ijkplayer 在选择硬解码器时，会通过 {@link android.media.MediaCodecList} 枚举设备上所有解码器，
 * 然后用本类的 {@link #setupCandidate} 方法对每个候选解码器打分（mRank），
 * 最终选出分数最高且不低于 {@link #RANK_LAST_CHANCE} 的解码器交给 MediaCodec 使用。</p>
 */
public class IjkMediaCodecInfo {
    private final static String TAG = "IjkMediaCodecInfo";

    // -----------------------------------------------------------------------
    // Rank 等级定义（值越大越优先）
    // -----------------------------------------------------------------------

    /** 预留最高分，当前未使用 */
    public static final int RANK_MAX = 1000;

    /**
     * 经过测试验证、可以放心使用的已知硬件解码器。
     * 白名单 {@link #sKnownCodecList} 中收录的主流芯片厂商解码器默认为此级别。
     */
    public static final int RANK_TESTED = 800;

    /**
     * 未在白名单中，但系统能成功返回 {@link CodecCapabilities}，认为可以接受。
     * 主要覆盖白名单之外的新设备硬件解码器。
     */
    public static final int RANK_ACCEPTABLE = 700;

    /**
     * 能力查询失败（getCapabilitiesForType 抛异常或返回 null），作为最后手段尝试。
     * rank 低于此值的解码器将被直接拒绝，回退到软解。
     */
    public static final int RANK_LAST_CHANCE = 600;

    /**
     * 带 .secure 后缀的安全解码器，功能受限，优先级低于普通硬解。
     */
    public static final int RANK_SECURE = 300;

    /**
     * 软件解码器（OMX.google.*、OMX.ffmpeg.* 等），能用但性能差，优先级低。
     */
    public static final int RANK_SOFTWARE = 200;

    /**
     * 非标准解码器（名称既不以 omx. 也不以 c2. 开头，仅 API 29 以下的兜底逻辑会用到此分档），
     * 兼容性未知，几乎不会被选中。
     */
    public static final int RANK_NON_STANDARD = 100;

    /**
     * 已知有问题或明确排除的解码器（如 OMX.ittiam.*），不参与选择。
     */
    public static final int RANK_NO_SENSE = 0;

    // -----------------------------------------------------------------------
    // 成员字段
    // -----------------------------------------------------------------------

    /** 系统原始的 MediaCodecInfo 对象 */
    public MediaCodecInfo mCodecInfo;

    /** 当前解码器的评分，由 {@link #setupCandidate} 计算得出 */
    public int mRank = 0;

    /** 当前解码器对应的 MIME 类型，如 "video/avc" */
    public String mMimeType;

    // -----------------------------------------------------------------------
    // 已知解码器白名单
    // -----------------------------------------------------------------------

    /**
     * 已知解码器白名单，Key 为 codec 名称（大小写不敏感），Value 为对应 Rank。
     * 收录了 2013-2015 年主流芯片厂商的已验证解码器，以及需要排除的软解/问题解码器。
     * 懒加载，首次调用时初始化。
     */
    private static Map<String, Integer> sKnownCodecList;

    /**
     * 获取已知解码器白名单（线程安全，懒加载）。
     *
     * <p>白名单收录规则：</p>
     * <ul>
     *   <li>已验证稳定的硬件解码器 → {@link #RANK_TESTED}(800)</li>
     *   <li>已知仅为软件实现 → {@link #RANK_SOFTWARE}(200)</li>
     *   <li>已知有严重问题 → {@link #RANK_NO_SENSE}(0)</li>
     *   <li>尚未验证的用 remove() 标注，不纳入白名单（留给通用流程处理）</li>
     * </ul>
     */
    private static synchronized Map<String, Integer> getKnownCodecList() {
        if (sKnownCodecList != null)
            return sKnownCodecList;

        // TreeMap 使用大小写不敏感排序，避免厂商命名大小写差异导致查找失败
        sKnownCodecList = new TreeMap<String, Integer>(
                String.CASE_INSENSITIVE_ORDER);

        // ----- Nvidia -----
        // Tegra3: Nexus 7 (2012)
        // Tegra K1: Nexus 9
        sKnownCodecList.put("OMX.Nvidia.h264.decode", RANK_TESTED);
        // 带 .secure 后缀的安全解码器，优先级低于普通硬解
        sKnownCodecList.put("OMX.Nvidia.h264.decode.secure", RANK_SECURE);

        // ----- Intel -----
        // Atom Z3735: Teclast X98 Air（+1 使其在同分情况下优先）
        sKnownCodecList.put("OMX.Intel.hw_vd.h264", RANK_TESTED + 1);
        // Atom Z2560: Dell Venue 7 3730
        sKnownCodecList.put("OMX.Intel.VideoDecoder.AVC", RANK_TESTED);

        // ----- Qualcomm -----
        // MSM8260: Xiaomi MI 1S
        sKnownCodecList.put("OMX.qcom.video.decoder.avc", RANK_TESTED);
        // ittiam 是高通平台上的第三方软解，已知有问题，排除
        sKnownCodecList.put("OMX.ittiam.video.decoder.avc", RANK_NO_SENSE);

        // ----- Samsung -----
        // Exynos 3110: Nexus S
        // 三个 SEC codec 稳定性递减，分别差 1 分加以区分
        sKnownCodecList.put("OMX.SEC.avc.dec", RANK_TESTED);
        sKnownCodecList.put("OMX.SEC.AVC.Decoder", RANK_TESTED - 1);
        // GT-9100 上 OMX.SEC.avcdec 不会对输出帧重排序，稳定性略差
        sKnownCodecList.put("OMX.SEC.avcdec", RANK_TESTED - 2);
        // SW 后缀说明是软件实现
        sKnownCodecList.put("OMX.SEC.avc.sw.dec", RANK_SOFTWARE);
        // Exynos 5 系列
        sKnownCodecList.put("OMX.Exynos.avc.dec", RANK_TESTED);
        sKnownCodecList.put("OMX.Exynos.AVC.Decoder", RANK_TESTED - 1);

        // ----- 华为 HiSilicon -----
        // 麒麟 910 + Mali 450 MP: Huawei HONOR 3C (H30-L01)
        sKnownCodecList.put("OMX.k3.video.decoder.avc", RANK_TESTED);
        // 麒麟 920 + Mali T624: Huawei HONOR 6
        sKnownCodecList.put("OMX.IMG.MSVDX.Decoder.AVC", RANK_TESTED);

        // ----- TI -----
        // TI OMAP4460: Galaxy Nexus
        sKnownCodecList.put("OMX.TI.DUCATI1.VIDEO.DECODER", RANK_TESTED);

        // ----- RockChip -----
        // 优酷电视盒
        sKnownCodecList.put("OMX.rk.video_decoder.avc", RANK_TESTED);

        // ----- AMLogic -----
        // 小米盒子 1 / 1s / 2
        sKnownCodecList.put("OMX.amlogic.avc.decoder.awesome", RANK_TESTED);

        // ----- Marvell -----
        // Lenovo A788t
        sKnownCodecList.put("OMX.MARVELL.VIDEO.HW.CODA7542DECODER", RANK_TESTED);
        // Marvell 的软件解码器
        sKnownCodecList.put("OMX.MARVELL.VIDEO.H264DECODER", RANK_SOFTWARE);

        // ----- 尚未验证，暂不纳入白名单 -----
        // 这些 codec 使用 remove() 而非 put()，是因为之前可能被批量 put 进来，
        // 这里明确排除，让它们走通用的 getCapabilitiesForType() 流程
        sKnownCodecList.remove("OMX.Action.Video.Decoder");
        sKnownCodecList.remove("OMX.allwinner.video.decoder.avc");
        sKnownCodecList.remove("OMX.BRCM.vc4.decoder.avc");
        sKnownCodecList.remove("OMX.brcm.video.h264.hw.decoder");
        sKnownCodecList.remove("OMX.brcm.video.h264.decoder");
        sKnownCodecList.remove("OMX.cosmo.video.decoder.avc");
        sKnownCodecList.remove("OMX.duos.h264.decoder");
        sKnownCodecList.remove("OMX.hantro.81x0.video.decoder");
        sKnownCodecList.remove("OMX.hantro.G1.video.decoder");
        sKnownCodecList.remove("OMX.hisi.video.decoder");
        sKnownCodecList.remove("OMX.LG.decoder.video.avc");
        sKnownCodecList.remove("OMX.MS.AVC.Decoder");
        sKnownCodecList.remove("OMX.RENESAS.VIDEO.DECODER.H264");
        sKnownCodecList.remove("OMX.RTK.video.decoder");
        sKnownCodecList.remove("OMX.sprd.h264.decoder");
        sKnownCodecList.remove("OMX.ST.VFM.H264Dec");
        sKnownCodecList.remove("OMX.vpu.video_decoder.avc");
        sKnownCodecList.remove("OMX.WMT.decoder.avc");

        // BlueStacks 模拟器的伪硬解，排除
        sKnownCodecList.remove("OMX.bluestacks.hw.decoder");

        // ----- 已知软件解码器，明确降级 -----
        // Google 官方软解
        sKnownCodecList.put("OMX.google.h264.decoder", RANK_SOFTWARE);
        sKnownCodecList.put("OMX.google.h264.lc.decoder", RANK_SOFTWARE);
        // 华为 K920 上基于 FFmpeg 的软解
        sKnownCodecList.put("OMX.k3.ffmpeg.decoder", RANK_SOFTWARE);
        sKnownCodecList.put("OMX.ffmpeg.video.decoder", RANK_SOFTWARE);
        // 展讯软解
        sKnownCodecList.put("OMX.sprd.soft.h264.decoder", RANK_SOFTWARE);

        return sKnownCodecList;
    }

    // -----------------------------------------------------------------------
    // 核心：评分并创建候选对象
    // -----------------------------------------------------------------------

    /**
     * 对一个 {@link MediaCodecInfo} 进行评分，返回包装后的候选对象。
     *
     * <p>评分策略（优先级从高到低）：</p>
     * <ol>
     *   <li>命中已知问题解码器（如 ittiam）直接判 {@link #RANK_NO_SENSE}，与命名体系无关</li>
     *   <li>Android 10 (API 29) 及以上：使用官方 {@link MediaCodecInfo#isHardwareAccelerated()} /
     *       {@link MediaCodecInfo#isSoftwareOnly()} / {@link MediaCodecInfo#isVendor()} 判断，
     *       不再依赖名称前缀匹配 —— 天然兼容 Codec2（c2.*）命名的现代硬解，也不需要再维护一份
     *       按厂商/型号硬编码的白名单</li>
     *   <li>低于 API 29：退回旧的按名称前缀启发式判断（omx.google.、omx.mtk. 等 + 白名单 +
     *       {@link CodecCapabilities} 探测），覆盖老设备</li>
     * </ol>
     *
     * @param codecInfo 系统解码器信息
     * @param mimeType  目标视频 MIME 类型，如 "video/avc"
     * @return 评分后的候选对象，不可用时返回 null
     */
    @TargetApi(Build.VERSION_CODES.Q)
    public static IjkMediaCodecInfo setupCandidate(MediaCodecInfo codecInfo, String mimeType) {
        // Android 4.1 以下不支持 MediaCodec API
        if (codecInfo == null
                || Build.VERSION.SDK_INT < Build.VERSION_CODES.JELLY_BEAN)
            return null;

        String name = codecInfo.getName();
        if (TextUtils.isEmpty(name))
            return null;

        // 统一转小写，消除厂商命名大小写差异
        name = name.toLowerCase(Locale.US);

        int rank;
        if (name.contains("ittiam")) {
            // 高通平台上的第三方软解，已知有问题，无论新旧命名体系都要排除
            rank = RANK_NO_SENSE;
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            rank = rankByPlatformApi(codecInfo, name);
        } else {
            rank = rankByNameHeuristic(codecInfo, name, mimeType);
        }

        IjkMediaCodecInfo candidate = new IjkMediaCodecInfo();
        candidate.mCodecInfo = codecInfo;
        candidate.mRank = rank;
        candidate.mMimeType = mimeType;
        return candidate;
    }

    /**
     * Android 10 (API 29) 及以上的评分策略：直接使用平台 API 判断，不再猜名字。
     *
     * <p>这一档同时覆盖了老式 OMX.* 和新式 Codec2 c2.* 命名 —— 只要平台认为它是硬件解码器即可，
     * 完全不用关心具体命名规则，也不需要为每一代新芯片补充白名单。</p>
     */
    @TargetApi(Build.VERSION_CODES.Q)
    private static int rankByPlatformApi(MediaCodecInfo codecInfo, String lowerCaseName) {
        int rank;
        if (codecInfo.isSoftwareOnly()) {
            // 平台明确认定为纯软件实现（含 Google/FFmpeg 等软解）
            rank = RANK_SOFTWARE;
        } else if (codecInfo.isHardwareAccelerated()) {
            // 厂商提供的真硬件解码器优先于平台内置但未标注厂商的实现
            rank = codecInfo.isVendor() ? RANK_TESTED : RANK_ACCEPTABLE;
        } else {
            // 既未标记纯软件也未标记硬件加速，存疑但仍作为最后手段尝试
            rank = RANK_LAST_CHANCE;
        }

        if (lowerCaseName.endsWith(".secure")) {
            // 安全解码器功能受限（仅用于 DRM 保护内容），优先级不应超过普通硬解
            rank = Math.min(rank, RANK_SECURE);
        }
        return rank;
    }

    /**
     * 低于 API 29 时的兜底策略：按名称前缀启发式判断 + 白名单 + {@link CodecCapabilities} 探测。
     * 这是老设备（Codec2 尚未普及）的历史逻辑，原样保留。
     */
    private static int rankByNameHeuristic(MediaCodecInfo codecInfo, String name, String mimeType) {
        int rank;
        if (!name.startsWith("omx.") && !name.startsWith("c2.")) {
            // 既非 OMX 也非 Codec2 标准命名，兼容性未知
            rank = RANK_NON_STANDARD;
        } else if (name.startsWith("omx.pv")) {
            // PacketVideo 软解（Android 早期内置）
            rank = RANK_SOFTWARE;
        } else if (name.startsWith("omx.google.") || name.startsWith("c2.android.")) {
            // Google/AOSP 官方软解，性能差但通用
            rank = RANK_SOFTWARE;
        } else if (name.startsWith("omx.ffmpeg.")) {
            // 基于 FFmpeg 的软解
            rank = RANK_SOFTWARE;
        } else if (name.startsWith("omx.k3.ffmpeg.")) {
            // 华为 K920 上基于 FFmpeg 的软解
            rank = RANK_SOFTWARE;
        } else if (name.startsWith("omx.avcodec.")) {
            // 另一类 FFmpeg/avcodec 软解
            rank = RANK_SOFTWARE;
        } else if (name.startsWith("omx.mtk.")) {
            // 联发科 MTK 硬解：
            // 1. Android 4.3 以下有兼容问题
            // 2. MIUI 6 (4.2.1) 上特殊适配可用
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.JELLY_BEAN_MR2)
                rank = RANK_NO_SENSE;
            else
                rank = RANK_TESTED;
        } else {
            // 其他 omx.*/c2.* 解码器：先查白名单，再用 API 探测
            Integer knownRank = getKnownCodecList().get(name);
            if (knownRank != null) {
                // 命中白名单，直接使用预设 rank
                rank = knownRank;
            } else {
                // 白名单未收录，尝试通过系统 API 探测解码能力
                try {
                    CodecCapabilities cap = codecInfo.getCapabilitiesForType(mimeType);
                    if (cap != null)
                        rank = RANK_ACCEPTABLE;  // 系统确认支持该 mimeType
                    else
                        rank = RANK_LAST_CHANCE; // 返回 null，存疑但仍尝试
                } catch (Throwable e) {
                    // 部分厂商实现有 bug，getCapabilitiesForType 可能抛异常
                    rank = RANK_LAST_CHANCE;
                }
            }
        }

        if (name.endsWith(".secure") && rank > RANK_SECURE) {
            // 安全解码器功能受限，优先级不应超过普通硬解（白名单里已单独配置的除外）
            rank = RANK_SECURE;
        }

        return rank;
    }

    // -----------------------------------------------------------------------
    // 调试工具
    // -----------------------------------------------------------------------

    /**
     * 打印该解码器支持的最高 Profile/Level 信息（仅用于调试日志）。
     *
     * @param mimeType 目标 MIME 类型
     */
    @TargetApi(Build.VERSION_CODES.JELLY_BEAN)
    public void dumpProfileLevels(String mimeType) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.JELLY_BEAN)
            return;

        try {
            CodecCapabilities caps = mCodecInfo.getCapabilitiesForType(mimeType);
            int maxProfile = 0;
            int maxLevel = 0;
            if (caps != null) {
                if (caps.profileLevels != null) {
                    for (CodecProfileLevel profileLevel : caps.profileLevels) {
                        if (profileLevel == null)
                            continue;
                        maxProfile = Math.max(maxProfile, profileLevel.profile);
                        maxLevel = Math.max(maxLevel, profileLevel.level);
                    }
                }
            }
            Log.i(TAG, String.format(Locale.US, "%s",
                    getProfileLevelName(maxProfile, maxLevel)));
        } catch (Throwable e) {
            Log.i(TAG, "profile-level: exception");
        }
    }

    /**
     * 将 profile/level 整数值格式化为可读字符串，如 "High Profile Level 4.1 (8,2048)"。
     */
    public static String getProfileLevelName(int profile, int level) {
        return String.format(Locale.US, " %s Profile Level %s (%d,%d)",
                getProfileName(profile), getLevelName(level), profile, level);
    }

    /**
     * 将 AVC Profile 整数常量转为可读名称。
     */
    public static String getProfileName(int profile) {
        switch (profile) {
        case CodecProfileLevel.AVCProfileBaseline:
            return "Baseline";
        case CodecProfileLevel.AVCProfileMain:
            return "Main";
        case CodecProfileLevel.AVCProfileExtended:
            return "Extends";
        case CodecProfileLevel.AVCProfileHigh:
            return "High";
        case CodecProfileLevel.AVCProfileHigh10:
            return "High10";
        case CodecProfileLevel.AVCProfileHigh422:
            return "High422";
        case CodecProfileLevel.AVCProfileHigh444:
            return "High444";
        default:
            return "Unknown";
        }
    }

    /**
     * 将 AVC Level 整数常量转为可读名称，如 AVCLevel41 → "4.1"。
     * AVCLevel52 (65536) 在旧版 SDK 中没有符号常量，直接用数值判断。
     */
    public static String getLevelName(int level) {
        switch (level) {
        case CodecProfileLevel.AVCLevel1:
            return "1";
        case CodecProfileLevel.AVCLevel1b:
            return "1b";
        case CodecProfileLevel.AVCLevel11:
            return "11";
        case CodecProfileLevel.AVCLevel12:
            return "12";
        case CodecProfileLevel.AVCLevel13:
            return "13";
        case CodecProfileLevel.AVCLevel2:
            return "2";
        case CodecProfileLevel.AVCLevel21:
            return "21";
        case CodecProfileLevel.AVCLevel22:
            return "22";
        case CodecProfileLevel.AVCLevel3:
            return "3";
        case CodecProfileLevel.AVCLevel31:
            return "31";
        case CodecProfileLevel.AVCLevel32:
            return "32";
        case CodecProfileLevel.AVCLevel4:
            return "4";
        case CodecProfileLevel.AVCLevel41:
            return "41";
        case CodecProfileLevel.AVCLevel42:
            return "42";
        case CodecProfileLevel.AVCLevel5:
            return "5";
        case CodecProfileLevel.AVCLevel51:
            return "51";
        case 65536: // CodecProfileLevel.AVCLevel52（旧版 SDK 无此常量）
            return "52";
        default:
            return "0";
        }
    }
}