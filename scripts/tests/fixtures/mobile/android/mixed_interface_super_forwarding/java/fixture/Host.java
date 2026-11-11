package fixture;
public class Host {
    public abstract static class Base implements Shared {
        protected Base() {}
        @Override public int base() {
            Trace.baseOwner = this;
            Trace.hit(3);
            if (Trace.baseAction != null) Trace.baseAction.run();
            return Trace.baseValue;
        }
    }
}
