package fixture; public final class Trace {
 public static Object partial,seed;public static Input output;public static float first,second;public static RuntimeException failure;public static int failAt;public static long events;
 public static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
 public static Input create(Bound seed,float first,float second){Trace.seed=seed;Trace.first=first;Trace.second=second;hit(1);return output;}
}
