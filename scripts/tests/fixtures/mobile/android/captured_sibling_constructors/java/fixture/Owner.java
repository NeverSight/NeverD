package fixture;

public final class Owner<T, U> {
    public Owner() {}

    private abstract class Base {
        final Probe<? super T> sink;
        final int marker;

        Base(Probe<? super T> sink, int marker) {
            Trace.record(this, 1);
            this.marker = marker;
            Trace.record(this, 2);
            this.sink = sink;
            Trace.record(this, 3);
        }
    }

    private final class Child extends Base {
        Child(Probe<? super T> sink, int marker) {
            super(sink, marker);
        }
    }
}
