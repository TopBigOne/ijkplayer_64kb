package tv.danmaku.ijk.media.example.content;

import androidx.annotation.StringRes;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

import tv.danmaku.ijk.media.example.R;

/**
 * 示例 App「Sample」页的测试链接目录，按用途分类。
 * 新增链接时加到对应分类里即可，界面会自动展示；文案放在 strings_sample.xml（英文）
 * 与 values-zh/strings_sample.xml（中文），这里只存资源 ID。
 */
public final class SampleMediaCatalog {

    public static final class Item {
        @StringRes
        public final int nameRes;
        public final String url;

        Item(@StringRes int nameRes, String url) {
            this.nameRes = nameRes;
            this.url = url;
        }
    }

    public static final class Category {
        @StringRes
        public final int titleRes;
        @StringRes
        public final int descriptionRes;
        /** 卡片左侧徽标上的短标签，如 WS / HLS；各分类唯一，也用作保存自定义地址的 key。 */
        public final String badge;
        /** 分类主题色（ARGB），用于徽标和列表强调色。 */
        public final int color;
        /** 为 true 时列表顶部提供「输入自定义地址」入口，比如局域网 IP 不固定的 ws 推流。 */
        public final boolean allowCustomUrl;
        @StringRes
        public final int customUrlHintRes;
        /** 为 true 时点击分类直接打开本地文件浏览，而不是链接列表。 */
        public final boolean opensFileBrowser;
        public final List<Item> items;

        private Category(Builder b) {
            titleRes = b.titleRes;
            descriptionRes = b.descriptionRes;
            badge = b.badge;
            color = b.color;
            allowCustomUrl = b.customUrlHintRes != 0;
            customUrlHintRes = b.customUrlHintRes;
            opensFileBrowser = b.opensFileBrowser;
            items = Collections.unmodifiableList(b.items);
        }
    }

    private static final class Builder {
        final String badge;
        final int color;
        final int titleRes;
        final int descriptionRes;
        int customUrlHintRes;
        boolean opensFileBrowser;
        final List<Item> items = new ArrayList<>();

        Builder(String badge, int color, @StringRes int titleRes, @StringRes int descriptionRes) {
            this.badge = badge;
            this.color = color;
            this.titleRes = titleRes;
            this.descriptionRes = descriptionRes;
        }

        Builder customUrl(@StringRes int hintRes) {
            customUrlHintRes = hintRes;
            return this;
        }

        Builder fileBrowser() {
            opensFileBrowser = true;
            return this;
        }

        Builder add(@StringRes int nameRes, String url) {
            items.add(new Item(nameRes, url));
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
        list.add(new Builder("WS", 0xFF3370FF, R.string.sample_cat_ws_title, R.string.sample_cat_ws_desc)
                .customUrl(R.string.sample_cat_ws_custom_hint)
                .add(R.string.sample_ws_adb, "ws://127.0.0.1:18780/live/test.flv")
                .add(R.string.sample_wss_adb, "wss://127.0.0.1:18781/live/test.flv")
                .build());

        // 不需要额外服务端，验证 ijkws 的错误码与超时（见 README「错误码」）
        list.add(new Builder("ERR", 0xFFF54A45, R.string.sample_cat_err_title, R.string.sample_cat_err_desc)
                .add(R.string.sample_err_handshake, "ws://127.0.0.1:18780/live/not_exist.flv")
                .add(R.string.sample_err_refused, "ws://127.0.0.1:18799/live/test.flv")
                .add(R.string.sample_err_dns, "ws://ijkws-test.invalid/live/test.flv")
                .add(R.string.sample_err_tls, "wss://127.0.0.1:18780/live/test.flv")
                .build());

        list.add(new Builder("HLS", 0xFF00B578, R.string.sample_cat_hls_title, R.string.sample_cat_hls_desc)
                .add(R.string.sample_hls_bipbop_4x3, APPLE_HLS + "bipbop_4x3/bipbop_4x3_variant.m3u8")
                .add(R.string.sample_hls_bipbop_16x9, APPLE_HLS + "bipbop_16x9/bipbop_16x9_variant.m3u8")
                .add(R.string.sample_hls_adv_fmp4, APPLE_HLS + "img_bipbop_adv_example_fmp4/master.m3u8")
                .add(R.string.sample_hls_adv_hevc, APPLE_HLS + "bipbop_adv_example_hevc/master.m3u8")
                .add(R.string.sample_hls_mux, "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8")
                .build());

        list.add(new Builder("MP4", 0xFFFF8800, R.string.sample_cat_mp4_title, R.string.sample_cat_mp4_desc)
                .add(R.string.sample_mp4_bbb_h264, TEST_VIDEOS + "bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_5MB.mp4")
                .add(R.string.sample_mp4_bbb_hevc, TEST_VIDEOS + "bigbuckbunny/mp4/h265/1080/Big_Buck_Bunny_1080_10s_5MB.mp4")
                .add(R.string.sample_mp4_jellyfish, TEST_VIDEOS + "jellyfish/mp4/h264/1080/Jellyfish_1080_10s_5MB.mp4")
                .add(R.string.sample_mp4_sintel, "https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4")
                .add(R.string.sample_mp4_w3c_bunny, "https://media.w3.org/2010/05/bunny/movie.mp4")
                .build());

        list.add(new Builder("FILE", 0xFF14A3A3, R.string.sample_cat_file_title, R.string.sample_cat_file_desc)
                .fileBrowser()
                .build());

        list.add(new Builder("LAN", 0xFF7F3BF5, R.string.sample_cat_lan_title, R.string.sample_cat_lan_desc)
                .customUrl(R.string.sample_cat_lan_custom_hint)
                .add(R.string.sample_lan_samba, "http://10.18.17.49/samba/video/4.mp4")
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
