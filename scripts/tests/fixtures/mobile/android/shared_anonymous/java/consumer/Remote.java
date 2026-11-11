package consumer;

import fixture.SharedHost;

public class Remote {
    public static Object make(int input) { return new SharedHost.Worker(input); }
    public static int read(Object object) { return ((SharedHost.Worker) object).value; }
}
