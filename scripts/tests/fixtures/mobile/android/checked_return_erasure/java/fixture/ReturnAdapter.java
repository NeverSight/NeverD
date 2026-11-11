package fixture;

public final class ReturnAdapter implements Transform<Owner<?>, Object> {
    public static int calls;
    public ReturnAdapter() {}
    public static Result make(Owner<?> owner) {
        return new Result(owner.value, owner.stamp);
    }
    public Object apply(Owner<?> owner) {
        Object result = null;
        ++calls;
        boolean enabled = owner != null && owner.ready;
        if (enabled) {
            Object token = owner.read();
            result = new Result(token, owner.stamp);
        }
        return result;
    }
}
