import fixture.*;
public class CheckedReferenceReturnCaller {
    public static Result call(Api receiver, Object input, Box callback, long value) {
        return Client.call(receiver, input, callback, value);
    }
}
