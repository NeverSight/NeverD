package fixture;
public final class Factory {
    public static int events;
    public Token savedToken;
    public Callback savedCallback;
    public Context context;
    public Object tokenMark;
    public Bound produced, saved;
    public RuntimeException failure;
    public <T extends Bound> void accept(Token<T> token,
            Callback<? super Context, ? extends T> callback) {
        events = events * 5 + 1;
        savedToken = token; savedCallback = callback;
        tokenMark = token.inspect();
        Context snapshot = context; snapshot.touch();
        T value = callback.apply(snapshot); produced = value;
        if (failure != null) throw failure;
        saved = produced;
        events = events * 5 + 2;
    }
}
