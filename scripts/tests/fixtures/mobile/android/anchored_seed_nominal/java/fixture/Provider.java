package fixture;
import java.io.IOException;
public abstract class Provider {
    public abstract <T> Provider accept(Token<T> token, T value) throws IOException;
}
