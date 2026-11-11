package fixture;
import java.util.List;
public abstract class Policy implements Api {
    private final String message;
    public Policy(String message) { this.message = message; }
    public Void terminal(Scope scope, List<? extends Item> items, int size) {
        throw new IllegalStateException(((Object) message).toString());
    }
    @SuppressWarnings("unchecked")
    public int measure(Scope scope, List items, int size) {
        return ((Number) (Object) terminal(scope, items, size)).intValue();
    }
}
