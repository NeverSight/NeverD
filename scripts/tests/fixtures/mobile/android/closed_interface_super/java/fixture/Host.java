package fixture;
public class Host {
    public static class Child implements Contract.Api {
        @Override public Result choose(Item item) {
            if (item instanceof Special) {
                Trace.step(3);
                return new Result(this, (Special)item);
            }
            return Contract.Api.super.choose(item);
        }
    }
}
