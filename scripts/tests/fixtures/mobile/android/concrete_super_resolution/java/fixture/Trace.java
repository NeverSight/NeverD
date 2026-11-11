package fixture;
public final class Trace {
 public static long events;
 public static int failStep,commit;
 public static Throwable failure;
 public static boolean answer;
 public static Object input,other,seen,receiver;
 public static Child<?> created;
 public static void hit(int step) {
  events=events*10+step;
  if(step==1) input=other;
  if(step==2) commit=73;
  if(step==4) answer=!answer;
  if(step==failStep) {
   if(failure instanceof Error) throw (Error)failure;
   throw (RuntimeException)failure;
  }
 }
}
