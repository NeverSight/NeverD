package fixture;

public final class Scopes {
    public static <E> Object make(Token<E> token, Object tag, Action<E> action) {
        Holder<E> holder = new Holder<E>(token, tag);
        return action.apply(holder);
    }
    public static <Unused> Object make$default(Token token, Object tag, Action action, int mask, Object marker) {
        if ((mask & 1) != 0) {
            action = new Action<Object>() {
                public Token<Object> snapshot;
                public Object apply(Holder holder) {
                    snapshot = holder.token;
                    State.lastAction = this;
                    return holder.token.read();
                }
            };
        }
        if ((mask & 2) != 0) tag = null;
        Holder holder = new Holder(token, tag);
        return action.apply(holder);
    }
}
