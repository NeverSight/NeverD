package fixture;
public final class Core {
 public final Token value;public final long word;
 public Core(Token value,long word){Trace.partial=this;Trace.hit(5);this.value=value;Trace.hit(6);this.word=word;Trace.hit(7);}
 public Core(Iterable<?> input,long word,boolean marker){
  Token token=new Token(Trace.first(input),word);
  while(Trace.next(input)){token=new Token(Trace.step(input),word);}
  Trace.hit(3);
  while(Trace.next(input)){token=new Token(Trace.step(input),word);}
  Trace.hit(4);
  this(token,word);
 }
}
