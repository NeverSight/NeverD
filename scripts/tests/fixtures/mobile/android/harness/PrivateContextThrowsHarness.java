import fixture.*;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class PrivateContextThrowsHarness {
  private static void check(boolean value, String detail) {
    if (!value) throw new AssertionError("owned private context throws mismatch: " + detail);
  }
  private static Object invoke(int route, int operation, Core receiver, Context context, Carrier<Object> callback) throws Throwable {
    if (route == 0) return operation == 0 ? receiver.call(context, callback) : Core.callStatic(context, callback);
    if (route == 1) return PrivateContextThrowsCaller.invoke(receiver, operation, context, callback);
    String name = operation == 0 ? "call" : "callStatic";
    if (route == 2) {
      try { return Core.class.getMethod(name, Context.class, Carrier.class).invoke(operation == 0 ? receiver : null, context, callback); }
      catch (InvocationTargetException error) { throw error.getCause(); }
    }
    MethodType type = MethodType.methodType(Object.class, Context.class, Carrier.class);
    MethodHandle handle = operation == 0 ? MethodHandles.lookup().findVirtual(Core.class, name, type).bindTo(receiver)
                                        : MethodHandles.lookup().findStatic(Core.class, name, type);
    return handle.invokeExact(context, callback);
  }
  public static void main(String[] args) throws Throwable {
    int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
    for (int attempt = 0; attempt < 2; ++attempt) {
      final Core receiver = new Core();
      final ByteBuffer input = mode == 5 ? null : ByteBuffer.allocate(4);
      final Context context = new Context(input);
      final IOException checked = new IOException("checked"), cleanup = new IOException("cleanup");
      final RuntimeException runtime = new IllegalArgumentException("runtime"), post = new IllegalStateException("post");
      final Error error = new AssertionError("error");
      final Object result = mode == 1 ? null : mode == 2 ? new Object() : new String("payload");
      final int[] calls = {0};
      final ByteBuffer[] seen = {ByteBuffer.allocate(1)};
      Trace.events = 0; Trace.result = result; Trace.receiver = new Object();
      Trace.cleanup = mode == 9 || mode == 10 ? cleanup : null;
      Trace.post = mode == 11 ? post : null;
      Trace.action = mode == 12 ? new Runnable() { public void run() { context.input = null; Trace.result = new Object(); } } : null;
      Carrier<Object> callback = new Carrier<Object>() {
        public Object read(ByteBuffer value) throws IOException {
          Trace.hit(3); ++calls[0]; seen[0] = value;
          if (mode == 6 || mode == 10) throw checked;
          if (mode == 7) throw runtime;
          if (mode == 8) throw error;
          return Trace.result;
        }
      };
      Object returned = null; Throwable failure = null;
      try {
        returned = invoke(route, operation, receiver, mode == 3 || mode == 13 ? null : context,
                          mode == 4 || mode == 13 ? null : callback);
        Trace.hit(7);
        String text = (String)returned;
        check(text == result, "result identity");
      } catch (Throwable caught) { failure = caught; }
      long wantedTrace = mode == 3 || mode == 4 || mode == 13 ? 125L
                       : mode == 6 || mode == 7 || mode == 8 || mode == 10 ? 1235L
                       : mode == 9 ? 12345L : mode == 11 ? 123456L : 1234567L;
      check(Trace.events == wantedTrace, "trace " + Trace.events + " != " + wantedTrace);
      check(Trace.receiver == (operation == 0 ? receiver : null), "private receiver");
      int wantedCalls = mode == 3 || mode == 4 || mode == 13 ? 0 : 1;
      check(calls[0] == wantedCalls && (wantedCalls == 0 || seen[0] == input), "callback count/input identity");
      Throwable exact = mode == 6 ? checked : mode == 7 ? runtime : mode == 8 ? error
                      : mode == 9 || mode == 10 ? cleanup : mode == 11 ? post : null;
      if (exact != null) check(failure == exact, "exception identity");
      else if (mode == 2) check(failure instanceof ClassCastException && returned == result, "late cast");
      else if (mode == 3 || mode == 4 || mode == 13) check(failure instanceof NullPointerException, "null failure");
      else check(failure == null && returned == result, "returned identity");
      if (mode == 12) check(context.input == null && Trace.result != result, "cleanup aliases");
      System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
  }
}
