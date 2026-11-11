package fixture;
public final class Core {
 public static Target make(int x,int y,int distance) {
  return new Target(x << distance, y >> distance, x >>> distance, y << distance);
 }
 public static Target bias(int x,int y) {
  return new Target(x << 13, y >> -1, x >>> 32, y << 33);
 }
}
