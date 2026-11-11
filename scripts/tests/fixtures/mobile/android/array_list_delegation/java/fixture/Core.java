package fixture;
import java.util.Arrays;
import java.util.List;
public final class Core extends Parent<Bound> {
    public final Config config;
    public final List<? extends Carrier<? extends Bound>> values;
    public final boolean finished;
    public Core(Config config, List<? extends Carrier<? extends Bound>> values) {
        Trace.touch(1, this);
        this.config = config;
        Trace.latest = this;
        Trace.touch(2, this);
        this.values = values;
        Trace.touch(3, this);
        finished = true;
    }
    public Core(Config config, Carrier<? extends Bound>... values) {
        this(config, Arrays.asList(values));
    }
    public Core(List<? extends Carrier<? extends Bound>> values) {
        this(Config.DEFAULT, values);
    }
    public Core(Carrier<? extends Bound>... values) {
        this(Config.DEFAULT, values);
    }
}
