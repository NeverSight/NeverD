package fixture;
import java.io.IOException;
public final class Core {
    private Core() {}
    public static Provider receiver() { Trace.receivers++; Trace.hit(1); Trace.before.run(); return Trace.source; }
    public static <T> Provider call(String context, T value, Token<T, ?> token) throws IOException { return receiver().accept(context,value,token); }
    public static Provider checked(String context, Object value, Token<String, ?> token) throws IOException { return call(context,(String)value,token); }
    public static <T> Provider discard(String context, T value, Token<T, ?> token) throws IOException { receiver().accept(context,value,token); return Trace.sentinelResult; }
}
