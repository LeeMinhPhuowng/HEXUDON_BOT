import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * MiniJson — JSON parser tối giản cho bot HEXUDON (đủ dùng cho protocol).
 * Chỉ dùng JDK, không thư viện ngoài.
 */
public final class MiniJson {
    private final String s;
    private int i = 0;

    private MiniJson(String s) { this.s = s; }

    public static Object parse(String s) { return new MiniJson(s).value(); }

    // ==== Truy cập tiện dụng ====
    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Object o) { return o instanceof Map ? (Map<String, Object>) o : new HashMap<>(); }
    @SuppressWarnings("unchecked")
    public static List<Object> arr(Object o) { return o instanceof List ? (List<Object>) o : new ArrayList<>(); }
    public static String str(Object o) { return o instanceof String ? (String) o : ""; }
    public static int asInt(Object o) { return o instanceof Number ? ((Number) o).intValue() : 0; }
    public static Object get(Object o, String key) { return obj(o).get(key); }

    // ==== Parser ====
    private void ws() { while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++; }
    private char peek() { ws(); return s.charAt(i); }
    private void expect(char c) { if (peek() != c) throw new RuntimeException("json: mong '" + c + "' tai " + i); i++; }

    private Object value() {
        char c = peek();
        switch (c) {
            case '{': return object();
            case '[': return array();
            case '"': return string();
            case 't': i += 4; return Boolean.TRUE;
            case 'f': i += 5; return Boolean.FALSE;
            case 'n': i += 4; return null;
            default: return number();
        }
    }
    private Map<String, Object> object() {
        Map<String, Object> m = new HashMap<>();
        expect('{');
        if (peek() == '}') { i++; return m; }
        while (true) {
            String key = string();
            expect(':');
            m.put(key, value());
            if (peek() == ',') { i++; continue; }
            expect('}');
            return m;
        }
    }
    private List<Object> array() {
        List<Object> a = new ArrayList<>();
        expect('[');
        if (peek() == ']') { i++; return a; }
        while (true) {
            a.add(value());
            if (peek() == ',') { i++; continue; }
            expect(']');
            return a;
        }
    }
    private String string() {
        expect('"');
        StringBuilder b = new StringBuilder();
        while (s.charAt(i) != '"') {
            char c = s.charAt(i++);
            if (c == '\\') {
                char e = s.charAt(i++);
                switch (e) {
                    case 'n': b.append('\n'); break;
                    case 't': b.append('\t'); break;
                    case 'u': i += 4; b.append('?'); break; // bot không cần unicode
                    default: b.append(e);
                }
            } else b.append(c);
        }
        i++;
        return b.toString();
    }
    private Double number() {
        int start = i;
        while (i < s.length() && (Character.isDigit(s.charAt(i)) || "-+.eE".indexOf(s.charAt(i)) >= 0)) i++;
        return Double.parseDouble(s.substring(start, i));
    }
}
