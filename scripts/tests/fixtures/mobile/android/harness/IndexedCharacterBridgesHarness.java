import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class IndexedCharacterBridgesHarness {
    static int calls, failures;
    public static class Derived extends Text {
        public Derived(char[] values) { super(values); }
        @Override public char get(int index) { overrides++; return super.get(index); }
    }
    static void check(boolean value) {
        if (!value) throw new AssertionError("indexed character bridge contract");
    }
    static char call(int route, Text receiver, int index) throws Throwable {
        if (route == 0) return receiver.charAt(index);
        if (route == 1) return ((CharSequence)receiver).charAt(index);
        if (route == 2) return ((Indexed)receiver).charAt(index);
        if (route == 3) {
            try { return (Character)Text.class.getMethod("charAt", int.class).invoke(receiver,index); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandle entry = MethodHandles.lookup().findVirtual(Text.class,"charAt",MethodType.methodType(char.class,int.class));
        return (char)entry.invokeExact(receiver,index);
    }
    public static void main(String[] args) throws Throwable {
        Method entry = Text.class.getDeclaredMethod("charAt",int.class);
        boolean original = args[0].equals("original");
        check(entry.getReturnType() == char.class && Modifier.isFinal(entry.getModifiers()));
        check(entry.isBridge() == original && entry.isSynthetic() == (original && args[1].equals("synthetic")));
        for (Method method : Derived.class.getDeclaredMethods()) check(!method.getName().equals("charAt"));
        char[] codes = {0,0x7f,0xff,0x7fff,0x8000,0xd800,0xdfff,0xffff};
        for (int route=0;route<5;route++)
            for (boolean derived : new boolean[]{false,true})
                for (int mode=0;mode<13;mode++) {
                    char[] shared = codes.clone();
                    Text receiver = derived ? new Derived(shared) : new Text(shared);
                    check(((CharSequence)receiver).length()==8 && ((Indexed)receiver).length()==8);
                    check(((CharSequence)receiver).subSequence(0,8)==receiver && ((Indexed)receiver).subSequence(0,8)==receiver);
                    for (int attempt=0;attempt<2;attempt++) {
                        int index = mode<8 ? mode : mode==8 ? -1 : mode==9 ? 8 : 0;
                        char expected = (char)(codes[index>=0 && index<8 ? index : 0] ^ (attempt==0 ? 0 : 0x55ff));
                        if (index>=0 && index<8) shared[index]=expected;
                        receiver.values = mode==10 ? null : shared;
                        receiver.events=0; receiver.overrides=0; receiver.lastIndex=99;
                        RuntimeException failure = new IllegalStateException("owned-character-failure");
                        receiver.failure = mode>=8 ? failure : null;
                        char result=0; Throwable caught=null;
                        try { result=call(route, mode==12 ? null : receiver,index); }
                        catch (Throwable error) { caught=error; }
                        calls++;
                        if (mode>=8) {
                            failures++;
                            if (mode==11) check(caught==failure);
                            else check(caught!=null && caught.getClass()==(mode==8 || mode==9 ? ArrayIndexOutOfBoundsException.class : NullPointerException.class));
                        } else check(caught==null && (int)result==(int)expected);
                        check(receiver.calls==(mode==12 ? 0 : attempt+1));
                        check(receiver.lastIndex==(mode==12 ? 99 : index));
                        check(receiver.events==(mode==12 ? 0 : mode>=8 ? 1 : 12));
                        check(receiver.overrides==(derived && mode!=12 ? 1 : 0));
                    }
                }
        check(calls==260 && failures==100);
        System.out.println("calls="+calls+",failures="+failures+",unsigned-char-and-original-virtual-forwarding=matched");
    }
}
