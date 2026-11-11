package fixture;
import java.util.List;
public interface Api {
    default int measure(Scope scope, List<? extends Item> items, int size) { return 99; }
}
