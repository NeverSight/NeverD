package fixture;
public final class Child extends Owner {
    @Override public int measure(float value) { Trace.hit(3);return super.measure(value); }
    @Override public float measure(int value) { Trace.hit(3);return super.measure(value); }
    @Override public double measure(long value) { Trace.hit(3);return super.measure(value); }
}
