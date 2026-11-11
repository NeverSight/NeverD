import fixture.*;
public final class SeededClosedInputsCaller {
    public static Box<?> call(Object seed, Box<?> input, Fn<?,?> closed,
            Fn<?,?> callback, Producer<?> producer) {
        return Client.call(seed,input,closed,callback,producer);
    }
}
