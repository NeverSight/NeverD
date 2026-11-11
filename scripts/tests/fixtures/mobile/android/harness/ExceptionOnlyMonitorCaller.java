import fixture.Core;
import java.io.IOException;

public final class ExceptionOnlyMonitorCaller {
    public static void call(Core core, Object lock, int kind) throws IOException {
        if (kind==0) core.fail(); else Core.failLock(lock);
    }
}
