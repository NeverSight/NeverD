import fixture.Child;
import java.util.Set;

public final class CheckedInterfaceCaller {
    public static Child<String> make(Set<? extends String> input) {
        return new Child<String>(input);
    }
}
