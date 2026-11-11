import api.Bound;
import fixture.Child;
public final class ProtectedBoundedReceiverCaller { public static Bound call(Child receiver,Bound input) { return receiver.run(input); } }
