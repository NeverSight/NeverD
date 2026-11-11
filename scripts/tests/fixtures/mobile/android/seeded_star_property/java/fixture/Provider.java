package fixture;
import java.io.IOException;
public abstract class Provider {
    public abstract <T> Provider accept(String context, T value, Token<T, ?> token) throws IOException;
}
