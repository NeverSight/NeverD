package fixture;
import base.Parent;
import java.net.URI;
import java.net.URISyntaxException;
public class Child extends Parent<URI> {
    public URI make(Object value)throws URISyntaxException {return new URI(this.text(value));}
}
