package fixture;
public final class Child<K,V> extends Middle<K,V,K> {
 public Target make() {
  return new Target(buffer()[index()-2],buffer()[index()-1]);
 }
}
