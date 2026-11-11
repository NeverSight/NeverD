package fixture;
public final class Result {
    public Object value;
    public Context context;
    public Token token;
    public Result(Object value, Context context, Token token) {
        this.value = value; this.context = context; this.token = token;
    }
}
