package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Caller {
    public static Void narrow(View<?> view, Entry<?> entry) {
        return ((View) view).typedThrow((Entry) entry);
    }
    public static boolean wide(Api<?, ?> api, Object entry) {
        return ((Api) api).add(entry);
    }
    public static Void narrowAll(View<?> view, java.util.Collection<?> entries) {
        return ((View) view).throwMany((java.util.Collection) entries);
    }
    public static boolean wideAll(Api<?, ?> api, java.util.Collection<?> entries) {
        return ((Api) api).addAll(entries);
    }
    public static Void narrowScalar(View<?> view, Object input) {
        return ((View) view).throwScalar(input);
    }
    public static boolean wideScalar(Api<?, ?> api, Object input) {
        return ((Api) api).push(input);
    }
}
