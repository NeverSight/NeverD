package fixture;
import java.io.IOException;
public final class Core {
    private Core() {}
    public static Provider receiver() { Trace.receivers++; Trace.hit(1); Trace.before.run(); return Trace.source; }
    public static <T> Provider call(Token<T> token, T value) throws IOException { return receiver().accept(token,value); }
    public static Provider checked(Token<String> token, Object value) throws IOException { return call(token,(String)value); }
    public static <T> Provider discard(Token<T> token, T value) throws IOException { receiver().accept(token,value); return Trace.sentinelResult; }
}
