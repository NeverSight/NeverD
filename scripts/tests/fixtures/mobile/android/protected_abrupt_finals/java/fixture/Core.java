package fixture;
public class Core {
    public static Core latest;
    public final Object value;
    public int stage;
    public Core(boolean allowed, Object seed) {
        latest = this;
        stage = 1;
        if (allowed) {
            Object returned;
            try {
                returned = Trace.produce(this, seed);
            } catch (Checked failure) {
                throw new IllegalStateException(failure);
            }
            value = returned;
            stage = 2;
            Trace.after(this, returned);
            stage = 3;
        } else {
            Trace.reject(this);
            throw Trace.guardFailure;
        }
    }
}
