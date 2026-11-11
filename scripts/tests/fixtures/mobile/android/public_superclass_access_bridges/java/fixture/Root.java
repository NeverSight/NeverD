package fixture;
abstract class Root<P> extends Base {
    public abstract P value();
    @Override public void write(Object marker, int number, long word) {
        calls += 2;
        super.write(marker, number, word);
    }
    @Override public boolean flag(Object marker, int number, long word) {
        calls += 2;
        return super.flag(marker, number, word);
    }
    @Override public int scalar(Object marker, int number, long word) {
        calls += 2;
        return super.scalar(marker, number, word);
    }
    @Override public long wide(Object marker, int number, long word) {
        calls += 2;
        return super.wide(marker, number, word);
    }
    @Override public double decimal(Object marker, int number, long word) {
        calls += 2;
        return super.decimal(marker, number, word);
    }
    @Override public Object reference(Object marker, int number, long word) {
        calls += 2;
        return super.reference(marker, number, word);
    }
}
