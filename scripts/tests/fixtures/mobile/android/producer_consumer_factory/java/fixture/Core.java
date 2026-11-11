package fixture;
import java.util.*;
import fixture.api.Facade;
public final class Core {
 public static <T> List<T> call(Iterable<? extends T> input,Comparator<? super T> action) {
  return Facade.build(input,action);
 }
}
