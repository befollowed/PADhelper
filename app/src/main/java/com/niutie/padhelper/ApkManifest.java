package com.niutie.padhelper;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * 从 APK 的二进制 {@code AndroidManifest.xml} 里读 versionName。
 *
 * <p>为什么不在游戏进程里直接问 PackageManager：那需要 Context（onPackageReady
 * 时机太早，Application 还没创建），或者走 {@code ActivityThread} 这类隐藏 API。
 * 读自己进程的 APK 则完全可行 —— sourceDir 就是本进程的安装包，读它没有任何权限问题，
 * 也不依赖隐藏 API。
 *
 * <p>只做一件事：找到根元素 {@code manifest} 上名为 {@code versionName} 的属性并返回。
 * 解析失败返回 null（调用方按「版本未知 = 不受支持」处理）。
 */
final class ApkManifest {

    private static final int RES_XML_TYPE = 0x0003;
    private static final int RES_STRING_POOL_TYPE = 0x0001;
    private static final int RES_XML_START_ELEMENT_TYPE = 0x0102;
    private static final int UTF8_FLAG = 0x100;
    private static final int TYPE_STRING = 0x03;

    private ApkManifest() {
    }

    /** 读 APK 的 versionName；任何失败都返回 null。 */
    static String versionName(String apkPath) {
        ZipFile zf = null;
        try {
            zf = new ZipFile(apkPath);
            ZipEntry e = zf.getEntry("AndroidManifest.xml");
            if (e == null) {
                return null;
            }
            InputStream in = zf.getInputStream(e);
            ByteArrayOutputStream bos = new ByteArrayOutputStream(1 << 16);
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) {
                bos.write(buf, 0, n);
            }
            in.close();
            return parse(bos.toByteArray());
        } catch (Throwable t) {
            return null;
        } finally {
            if (zf != null) {
                try {
                    zf.close();
                } catch (Throwable ignored) {
                }
            }
        }
    }

    private static String parse(byte[] b) {
        if (b.length < 8 || u16(b, 0) != RES_XML_TYPE) {
            return null;
        }
        int off = u16(b, 2); // chunk header size，通常是 8
        String[] pool = null;
        while (off + 8 <= b.length) {
            int type = u16(b, off);
            int size = u32(b, off + 4);
            if (size <= 0 || off + size > b.length) {
                break;
            }
            if (type == RES_STRING_POOL_TYPE && pool == null) {
                pool = stringPool(b, off);
            } else if (type == RES_XML_START_ELEMENT_TYPE && pool != null) {
                String name = poolStr(pool, u32(b, off + 20));
                if ("manifest".equals(name)) {
                    String v = versionNameAttr(b, off, pool);
                    if (v != null) {
                        return v;
                    }
                }
            }
            off += size;
        }
        return null;
    }

    /** 在 start element 的属性表里找 versionName。 */
    private static String versionNameAttr(byte[] b, int off, String[] pool) {
        int attrStart = u16(b, off + 24);
        int attrSize = u16(b, off + 26);
        int attrCount = u16(b, off + 28);
        if (attrSize <= 0) {
            attrSize = 20;
        }
        int base = off + 16 + attrStart; // attrStart 相对 ResXMLTree_attrExt 起点
        for (int i = 0; i < attrCount; i++) {
            int p = base + i * attrSize;
            if (p + 20 > b.length) {
                break;
            }
            String an = poolStr(pool, u32(b, p + 4));
            if (!"versionName".equals(an)) {
                continue;
            }
            int dataType = b[p + 15] & 0xff;
            int data = u32(b, p + 16);
            return dataType == TYPE_STRING ? poolStr(pool, data) : null;
        }
        return null;
    }

    private static String[] stringPool(byte[] b, int off) {
        int headerSize = u16(b, off + 2);
        int count = u32(b, off + 8);
        int flags = u32(b, off + 16);
        int stringsStart = u32(b, off + 20);
        boolean utf8 = (flags & UTF8_FLAG) != 0;
        String[] out = new String[count];
        for (int i = 0; i < count; i++) {
            int so = off + stringsStart + u32(b, off + headerSize + i * 4);
            out[i] = utf8 ? utf8(b, so) : utf16(b, so);
        }
        return out;
    }

    private static String poolStr(String[] pool, int idx) {
        return idx >= 0 && idx < pool.length ? pool[idx] : null;
    }

    private static String utf16(byte[] b, int p) {
        int len = u16(b, p);
        int start = p + 2;
        if (start + len * 2 > b.length) {
            return null;
        }
        return new String(b, start, len * 2, java.nio.charset.StandardCharsets.UTF_16LE);
    }

    private static String utf8(byte[] b, int p) {
        int len16 = b[p] & 0xff;
        p++;
        if ((len16 & 0x80) != 0) {
            len16 = ((len16 & 0x7f) << 8) | (b[p] & 0xff);
            p++;
        }
        int len8 = b[p] & 0xff;
        p++;
        if ((len8 & 0x80) != 0) {
            len8 = ((len8 & 0x7f) << 8) | (b[p] & 0xff);
            p++;
        }
        if (p + len8 > b.length) {
            return null;
        }
        return new String(b, p, len8, java.nio.charset.StandardCharsets.UTF_8);
    }

    private static int u16(byte[] b, int p) {
        return (b[p] & 0xff) | ((b[p + 1] & 0xff) << 8);
    }

    private static int u32(byte[] b, int p) {
        return (b[p] & 0xff)
                | ((b[p + 1] & 0xff) << 8)
                | ((b[p + 2] & 0xff) << 16)
                | ((b[p + 3] & 0xff) << 24);
    }
}
