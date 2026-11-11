package fixture;
import java.util.List;
public class Registry implements Contract {
    public Object payload;
    public Carrier<?> stored;
    public Registry() {}
    @SuppressWarnings("unchecked")
    public <T> void consume(Token<T> token, Callback<? super List<? extends Carrier<?>>, ? extends Carrier<?>> callback) {
        Trace.hit(1, this, token, callback, null);
        token.read();
        List<? extends Carrier<?>> values = (List<? extends Carrier<?>>)payload;
        Carrier<?> result = callback.apply(values);
        Trace.hit(2, this, token, callback, result);
        stored = result;
        Trace.hit(3, this, token, callback, result);
    }
}
