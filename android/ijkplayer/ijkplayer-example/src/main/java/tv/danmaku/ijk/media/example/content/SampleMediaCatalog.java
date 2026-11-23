package tv.danmaku.ijk.media.example.content;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * 示例 App「Sample」页的测试链接目录，按用途分类。
 * 新增链接时加到对应分类里即可，界面会自动展示。
 */
public final class SampleMediaCatalog {

    public static final class Item {
        public final String name;
        public final String url;

        Item(String name, String url) {
            this.name = name;
            this.url = url;
        }
    }

    public static final class Category {
        public final String title;
        public final String description;
        /** 卡片左侧徽标上的短标签，如 WS / HLS。 */
        public final String badge;
        /** 分类主题色（ARGB），用于徽标和列表强调色。 */
        public final int color;
        /** 为 true 时列表顶部提供「输入自定义地址」入口，比如局域网 IP 不固定的 ws 推流。 */
        public final boolean allowCustomUrl;
        public final String customUrlHint;
        /** 为 true 时点击分类直接打开本地文件浏览，而不是链接列表。 */
        public final boolean opensFileBrowser;
        public final List<Item> items;

        private Category(Builder b) {
            title = b.title;
            description = b.description;
            badge = b.badge;
            color = b.color;
            allowCustomUrl = b.customUrlHint != null;
            customUrlHint = b.customUrlHint;
            opensFileBrowser = b.opensFileBrowser;
            items = Collections.unmodifiableList(b.items);
        }
    }

    private static final class Builder {
        final String badge;
        final int color;
        final String title;
        final String description;
        String customUrlHint;
        boolean opensFileBrowser;
        final List<Item> items = new ArrayList<>();

        Builder(String badge, int color, String title, String description) {
            this.badge = badge;
            this.color = color;
            this.title = title;
            this.description = description;
        }

        Builder customUrl(String hint) {
            customUrlHint = hint;
            return this;
        }

        Builder fileBrowser() {
            opensFileBrowser = true;
            return this;
        }

        Builder add(String name, String url) {
            items.add(new Item(name, url));
            return this;
        }

        Category build() {
            return new Category(this);
        }
    }

    private static final String APPLE_HLS = "https://devstreaming-cdn.apple.com/videos/streaming/examples/";
    private static final String TEST_VIDEOS = "https://test-videos.co.uk/vids/";

    private static final List<Category> CATEGORIES;

    static {
        List<Category> list = new ArrayList<>();

        // 依赖 tools/ws-live.sh 在 Mac 上起的推流环境（ws:18780 / wss:18781）
        list.add(new Builder("WS", 0xFF3370FF, "WebSocket 拉流",
                "本地推流环境 tools/ws-live.sh；127.0.0.1 需先 adb reverse，局域网请用自定义地址")
                .customUrl("ws://<Mac局域网IP>:18780/live/test.flv")
                .add("ws-flv（adb reverse）", "ws://127.0.0.1:18780/live/test.flv")
                .add("wss-flv（adb reverse，自签名证书）", "wss://127.0.0.1:18781/live/test.flv")
                .build());

        // 不需要额外服务端，验证 ijkws 的错误码与超时（见 README「错误码」）
        list.add(new Builder("ERR", 0xFFF54A45, "WebSocket 异常场景",
                "验证 onError 错误码：握手被拒、连接失败、域名解析失败、协议不匹配")
                .add("握手被拒：流不存在（推流环境回 502）", "ws://127.0.0.1:18780/live/not_exist.flv")
                .add("连接失败：端口无服务（-5）", "ws://127.0.0.1:18799/live/test.flv")
                .add("域名解析失败（-5）", "ws://ijkws-test.invalid/live/test.flv")
                .add("wss 连到明文 ws 端口（TLS 握手失败）", "wss://127.0.0.1:18780/live/test.flv")
                .build());

        list.add(new Builder("HLS", 0xFF00B578, "HLS 点播",
                "Apple / Mux 官方测试流，覆盖多码率切换、fMP4、HEVC")
                .add("Apple bipbop 4:3 多码率（TS）", APPLE_HLS + "bipbop_4x3/bipbop_4x3_variant.m3u8")
                .add("Apple bipbop 16:9 多码率（TS）", APPLE_HLS + "bipbop_16x9/bipbop_16x9_variant.m3u8")
                .add("Apple 高级示例（fMP4，H.264）", APPLE_HLS + "img_bipbop_adv_example_fmp4/master.m3u8")
                .add("Apple 高级示例（fMP4，HEVC）", APPLE_HLS + "bipbop_adv_example_hevc/master.m3u8")
                .add("Mux 测试流（Tears of Steel 多码率）", "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8")
                .build());

        list.add(new Builder("MP4", 0xFFFF8800, "HTTP 点播",
                "公网 MP4 文件，验证 https、拖动、H.264 / HEVC 解码")
                .add("Big Buck Bunny 1080p H.264（10 秒）", TEST_VIDEOS + "bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_5MB.mp4")
                .add("Big Buck Bunny 1080p HEVC（10 秒）", TEST_VIDEOS + "bigbuckbunny/mp4/h265/1080/Big_Buck_Bunny_1080_10s_5MB.mp4")
                .add("Jellyfish 1080p H.264（10 秒）", TEST_VIDEOS + "jellyfish/mp4/h264/1080/Jellyfish_1080_10s_5MB.mp4")
                .add("Sintel 预告片 720p", "https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4")
                .add("Big Buck Bunny（W3C，约 1 分钟）", "https://media.w3.org/2010/05/bunny/movie.mp4")
                .build());

        list.add(new Builder("FILE", 0xFF14A3A3, "本地视频",
                "浏览手机存储，选择本地文件播放")
                .fileBrowser()
                .build());

        list.add(new Builder("LAN", 0xFF7F3BF5, "局域网 / 自定义",
                "内网资源或手动输入任意地址（http / rtmp / ws / file 等）")
                .customUrl("http://<host>/path/to/video.mp4")
                .add("内网 samba 4.mp4", "http://10.18.17.49/samba/video/4.mp4")
                .build());

        // 按链接条数从少到多排序（本地视频没有链接，算 0 条排最前）；条数相同保持上面的书写顺序
        Collections.sort(list, new Comparator<Category>() {
            @Override
            public int compare(Category a, Category b) {
                return Integer.compare(a.items.size(), b.items.size());
            }
        });
        CATEGORIES = Collections.unmodifiableList(list);
    }

    private SampleMediaCatalog() {
    }

    public static List<Category> categories() {
        return CATEGORIES;
    }

    public static Category category(int index) {
        if (index < 0 || index >= CATEGORIES.size())
            return null;
        return CATEGORIES.get(index);
    }
}
