package fixture;
import java.util.List;
public final class Provider {
    private Provider() {}
    public static <T extends Mark> List<T> read(Context context, String key, Token<T> token) {
        Trace.hit(1, context, key, token, null);
        context.before(key, token);
        List<T> result = (List<T>)context.reply;
        Trace.hit(2, context, key, token, result);
        return result;
    }
}
