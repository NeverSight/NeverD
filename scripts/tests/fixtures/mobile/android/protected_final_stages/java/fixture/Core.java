package fixture;

public class Core {
    public final int first, choice, last;
    static { Trace.hit(9); Trace.seed = 100; }

    public Core(Provider provider, int selector) {
        first = provider.first();
        if (selector == 0) choice = provider.choose(2);
        else if (selector == 1) choice = provider.choose(3);
        else choice = provider.choose(4);
        try {
            try { provider.step(); }
            catch (IllegalArgumentException failure) {
                Trace.caught = failure;
                provider.recover();
            }
        } finally {
            provider.cleanup();
        }
        last = provider.last();
        Trace.published = this;
        Trace.hit(10);
    }
}
