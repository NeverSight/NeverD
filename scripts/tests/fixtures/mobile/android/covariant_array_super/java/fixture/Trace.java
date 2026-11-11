package fixture;
public final class Trace {
 public static long events;
 public static int failStep, failOccurrence, matching, started, completed;
 public static Throwable failure;
 public static Parent<?,?,?> parent;
 public static Path<?,?,?> partialElement, first;
 public static void hit(int step) {
  events=events*10+step;
  if(step==failStep && ++matching==failOccurrence) {
   if(failure instanceof Error) throw (Error)failure;
   throw (RuntimeException)failure;
  }
 }
}
