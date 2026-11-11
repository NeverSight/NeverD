import fixture.*;
public final class StatelessMethodContractsCaller {
    public static <T, U> T echo(Api self, Class<T> token, T value) { return self.echo(token, value); }
    public static Carrier<Number, String> inspect(Api self, Carrier<Number, String> value) { return self.inspect(value); }
}
