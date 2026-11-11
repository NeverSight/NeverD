import dependency.Base;
import dependency.Contract;
import dependency.Note;
import fixture.ClasspathApp;

public class ClasspathHarness {
    public static void main(String[] ignored) {
        Contract value = new ClasspathApp();
        for (int input : new int[]{Integer.MIN_VALUE, -9, 0, 11, Integer.MAX_VALUE})
            System.out.println(value.value(input));
        Base.Member member = new Base.Member();
        System.out.println(ClasspathApp.member(member) == member);
        Note note = ClasspathApp.class.getAnnotation(Note.class);
        System.out.println(note.value());
        System.out.println(java.util.Arrays.toString(note.names()));
    }
}
