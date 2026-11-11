package fixture;
public final class Core {
    public Child provider;
    public Value value;
    public void run(long number, int label, boolean enabled, double fraction) {
        provider.accept(number, label, value, enabled, fraction);
    }
}
