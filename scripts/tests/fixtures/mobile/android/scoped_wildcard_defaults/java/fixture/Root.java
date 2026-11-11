package fixture;
import java.util.Map;
public interface Root<A, B> { void putAll(Map<? extends A, ? extends B> entries); }
