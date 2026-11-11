package fixture;

import java.io.IOException;

public class Core {
    public synchronized void fail() throws IOException {
        Trace.probe.observe(this);
        throw new IOException("owned monitor failure");
    }
    public static void failLock(Object lock) throws IOException {
        synchronized (lock) {
            Trace.probe.observe(lock);
            throw new IOException("owned monitor failure");
        }
    }
}
