package fixture;
public class Host {
    public abstract static class Base implements Shared {
        public long value;
        protected Base() { value = Trace.seed; }
        public void base() {
            Trace.baseOwner = this;
            Trace.before = value;
            Trace.hit(2);
            if (Trace.baseAction != null) Trace.baseAction.run();
            value += Trace.delta;
            Trace.observed = value;
        }
    }
}
