package fixture;
public final class Creator {
    public Target make(Object seed) {
        return new Target(seed, seed != null ? (int) Math.ceil((double) Provider.read(seed)) : 0,
                Provider.after());
    }
}
