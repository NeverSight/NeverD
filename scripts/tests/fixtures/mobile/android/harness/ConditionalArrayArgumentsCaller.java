import fixture.Core;
import fixture.Result;
import java.io.IOException;

public final class ConditionalArrayArgumentsCaller {
    public static Result make(boolean flag, int size, Object left, Object right) throws IOException {
        return Core.make(flag, size, left, right);
    }
}
