package fixture;
import java.util.List;
public final class Core {
    private Core() {}
    public static List<Leaf> run(Object owner, Object name, Object type) {
        Context context = (Context)owner;
        String key = (String)name;
        Token token = (Token)type;
        Trace.hit(3, context, key, token, null);
        List result = Provider.read(context, key, token);
        Trace.hit(4, context, key, token, result);
        return (List<Leaf>)result;
    }
    public static void ignore(Object owner, Object name, Object type) {
        Context context = (Context)owner;
        String key = (String)name;
        Token token = (Token)type;
        Trace.hit(3, context, key, token, null);
        Provider.read(context, key, token);
        Trace.hit(4, context, key, token, null);
    }
}
