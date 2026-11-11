package fixture;
public final class Child extends Owner {
    @Override public int measure(float value) { Trace.hit(3);return super.measure(value); }
}
