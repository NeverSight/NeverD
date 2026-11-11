package fixture;
import java.util.List;
public interface Contract {
    <T> void consume(Token<T> token, Callback<? super List<? extends Carrier<?>>, ? extends Carrier<?>> callback);
}
