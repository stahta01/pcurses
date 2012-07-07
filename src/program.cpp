/* *************************************************************************
 *  Copyright 2010 Jakob Gruber                                            *
 *                                                                         *
 *  This program is free software: you can redistribute it and/or modify   *
 *  it under the terms of the GNU General Public License as published by   *
 *  the Free Software Foundation, either version 3 of the License, or      *
 *  (at your option) any later version.                                    *
 *                                                                         *
 *  This program is distributed in the hope that it will be useful,        *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *  GNU General Public License for more details.                           *
 *                                                                         *
 *  You should have received a copy of the GNU General Public License      *
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.  *
 ************************************************************************* */

#include "program.h"

static volatile bool want_resize = false;
void request_resize(int /* unused */)
{
    want_resize = true;
}

Program::Program()
{
    quit = false;
    op = OP_NONE;
    mode = MODE_STANDARD;
    sortedby = A_NAME;
    coloredby = A_INSTALLSTATE;

    signal(SIGWINCH, request_resize);
}

Program::~Program()
{
    deinit();
}

void Program::do_resize()
{
    want_resize = false;

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    ensureminwsize(w.ws_col, w.ws_row);

    endwin();
    refresh();

    list_pane->reposition(w.ws_col, w.ws_row);
    info_pane->reposition(w.ws_col, w.ws_row);
    queue_pane->reposition(w.ws_col, w.ws_row);
    status_pane->reposition(w.ws_col, w.ws_row);
    input_pane->reposition(w.ws_col, w.ws_row);
    help_pane->reposition(w.ws_col, w.ws_row);

    updatedisplay();
}

void Program::ensureminwsize(uint w, uint h) const
{
    const uint minw = 60;
    const uint minh = 20;

    if (w < minw || h < minh) {
        throw PcursesException("Window size is below required minimum");
    }
}

void Program::deinit()
{
    deinit_curses();

    for (uint i = 0; i < packages.size(); i++)
        delete packages[i];

    filteredpackages.clear();
    packages.clear();
    opqueue.clear();
}

void Program::run_cmd(string cmd) const
{
    pid_t pid;
    int status;


    pid = fork();
    if (pid == 0) {
        /* child */
        execlp("bash", "bash", "-ic", cmd.c_str(), (char *)NULL);
    } else {
        /* parent (or error, which we blissfully ignore */

        /* ignore SIGTTOU (prevent getting sent to the background
          after run_cmd) */
        __sighandler_t handler = signal(SIGTTOU, SIG_IGN);

        waitpid(pid, &status, 0);
        tcsetpgrp(STDIN_FILENO, getpgid(0));    /* regain control of the terminal */
        std::cout << "press return to continue...";
        std::cin.get();

        /* and restore the original handler when done */
        signal(SIGTTOU, handler);
    }
}

void Program::init_misc()
{
    colorcodepackages(string(1, AttributeInfo::attrtochar(coloredby)));
    searchphrases.clear();

    /* exec startup macro if it exists */
    execmacro("startup");
}

void Program::init()
{
    loadpkgs();

    init_curses();
    init_misc();

    updatedisplay();
}

void Program::setfocus(CursesListBox *frame)
{
    list_pane->setfocused(false);
    queue_pane->setfocused(false);

    if (opqueue.empty() && frame == queue_pane) {
        focused_pane = list_pane;
    } else {
        focused_pane = frame;
    }

    focused_pane->setfocused(true);
}

void Program::mainloop()
{
    int ch;
    while (!quit) {
        ch = getch();

        if (want_resize) {
            do_resize();
        }

        if (ch == ERR || ch == KEY_RESIZE) continue;

        if (mode == MODE_STANDARD) {
            switch (ch) {
            case 'k':
            case KEY_UP:
                focused_pane->move(-1);
                break;
            case 'j':
            case KEY_DOWN:
                focused_pane->move(1);
                break;
            case KEY_HOME:
                focused_pane->moveabs(0);
                break;
            case KEY_END:
                focused_pane->movetoend();
                break;
            case KEY_PPAGE:   /* page up */
                focused_pane->move(list_pane->usableheight() * -1);
                break;
            case KEY_NPAGE:   /* page down */
                focused_pane->move(list_pane->usableheight());
                break;
            case KEY_TAB:
                setfocus((focused_pane == list_pane) ? queue_pane : list_pane);
                break;
            case KEY_RIGHT:
                if (focused_pane != list_pane) break;
                if (filteredpackages.size() == 0) break;

                if (std::find(opqueue.begin(), opqueue.end(), filteredpackages[list_pane->focusedindex()]) != opqueue.end()) {
                    break;
                }
                opqueue.push_back(filteredpackages[list_pane->focusedindex()]);
                queue_pane->movetoend();
                focused_pane->move(1);
                break;
            case KEY_LEFT:
                if (focused_pane != queue_pane) break;
                queue_pane->removeselected();
                if (opqueue.empty()) setfocus(list_pane);
                break;
            case 'C':
                while (!opqueue.empty()) {
                    queue_pane->removeselected();
                }
                setfocus(list_pane);
                break;
            case 'h':
                mode = MODE_HELP;
                break;
            case 'q':
                quit = true;
                break;
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '0':
                execmacro(string(1, ch));
                break;
            case 'r':
                deinit();
                init();
            case 'c':
                clearfilter();
                break;
            case 'n':
            case 'd':
                prepinputmode(OP_FILTER);
                inputbuf.set(string(1, ch) + ":");
                break;
            case '/':
            case '.':
            case '?':
            case ';':
            case '!':
            case '@':
                prepinputmode(strtoopt(string(1, ch)));
                break;
            default:
                break;
            }
        } else if (mode == MODE_INPUT) {
            switch (ch) {
            case KEY_ESC:
                exitinputmode(OP_NONE);
                break;
            case KEY_RETURN:
                exitinputmode(op);
                break;
            case KEY_DC:
                inputbuf.del();
                break;
            case KEY_BACKSPACE:
            case KEY_KONSOLEBACKSPACE:
                inputbuf.backspace();
                break;
            case KEY_LEFT:
                inputbuf.moveleft();
                break;
            case KEY_RIGHT:
                inputbuf.moveright();
                break;
            case KEY_HOME:
                inputbuf.movestart();
                break;
            case KEY_END:
                inputbuf.moveend();
                break;
            case KEY_UP:
                if (!gethis(op)->empty()) {
                    inputbuf.set(gethis(op)->moveback());
                }
                break;
            case KEY_DOWN:
                if (!gethis(op)->empty()) {
                    inputbuf.set(gethis(op)->moveforward());
                }
                break;
            default:
                inputbuf.insert(ch);
                break;
            }
        } else if (mode == MODE_HELP) {
            /* exit help screen with any key */
            mode = MODE_STANDARD;
        }

        updatedisplay();
    }
}

void Program::prepinputmode(FilterOperationEnum o)
{
    mode = MODE_INPUT;
    curs_set(1);
    inputbuf.clear();
    gethis(o)->reset();
    op = o;
}

void Program::exitinputmode(FilterOperationEnum o)
{
    mode = MODE_STANDARD;
    curs_set(0);

    op = OP_NONE;

    if (inputbuf.getcontents().length() == 0) {
        return;
    }

    switch (o) {
    case OP_FILTER:
        displayprocessingmsg();
        filterpackages(inputbuf.getcontents());
        list_pane->setlist(&filteredpackages);
        list_pane->moveabs(0);
        flushinp();
        break;
    case OP_SORT:
        sortpackages(inputbuf.getcontents());
        break;
    case OP_SEARCH:
        searchpackages(inputbuf.getcontents());
        break;
    case OP_COLORCODE:
        colorcodepackages(inputbuf.getcontents());
        break;
    case OP_EXEC:
        execmd(inputbuf.getcontents());
        break;
    case OP_MACRO:
        execmacro(inputbuf.getcontents());
        break;
    default:
        break;
    }
}

void Program::displayprocessingmsg()
{
    list_pane->setfooter("Processing...");
    updatedisplay();
}

#define PRINTH(a, b) help_pane->printw(a, A_BOLD); help_pane->printw(b);
void Program::print_help()
{
    PRINTH("esc: ", "cancel\n");
    PRINTH("q: ", "quit\n");
    PRINTH("1 to 0: ", "hotkeys (as configured in " APPLICATION_NAME ".conf)\n");
    PRINTH("!: ", "execute command, replacing %p with selected package names\n");
    PRINTH("@: ", "run the specified macro (as configured in " APPLICATION_NAME ".conf)\n");
    PRINTH("r: ", "reload package info\n");
    PRINTH("/: ", "filter packages by specified fields (using regexp)\n");
    PRINTH("", "   note that filters can be chained.\n")
    PRINTH("n: ", "filter packages by name (using regexp)\n");
    PRINTH("c: ", "clear all package filters\n");
    PRINTH("C: ", "clear the package queue\n");
    PRINTH("?: ", "search packages\n");
    PRINTH(".: ", "sort packages by specified field\n");
    PRINTH(";: ", "colorcode packages by specified field\n");
    PRINTH("tab: ", "switch focus between list and queue panes\n");
    PRINTH("left/right arrows: ", "add/remove packages from the queue\n");
    PRINTH("up/down arrows, pg up/down, home/end: ", "navigation\n");
    PRINTH("up/down arrows (in input mode): ", "browse history\n");
    help_pane->printw("\n");
    help_pane->printw("configure macros, hotkeys and hooks in " APPLICATION_NAME ".conf\n");
}
#undef PRINTH

void Program::loadpkgs()
{
    std::cout << "Reading package dbs, please wait..." << std::endl;

    _alpm_errno_t err;

    conf.parse_pacmanconf();
    conf.parse_pcursesconf();
    macros = conf.getmacros();


    alpm_handle_t *handle =
        alpm_initialize(conf.getrootdir().c_str(), conf.getdbpath().c_str(), &err);
    if (handle == NULL) {
        throw PcursesException(alpm_strerror(err));
    }

    alpm_option_set_logfile(handle, conf.getlogfile().c_str());

    vector<string> repos = conf.getrepos();
    for (uint i = 0; i < repos.size(); i++) {
        /* i'm going to be lazy here and remind myself to handle siglevel properly later on */
        alpm_db_register_sync(handle, repos[i].c_str(), ALPM_SIG_USE_DEFAULT);
    }

    alpm_db_t *localdb = alpm_option_get_localdb(handle);


    /* create our package list */
    alpm_list_t *dbs = alpm_list_copy(alpm_option_get_syncdbs(handle));
    dbs = alpm_list_add(dbs, localdb);
    for (; dbs; dbs = alpm_list_next(dbs)) {
        alpm_db_t *db = (alpm_db_t *)alpm_list_getdata(dbs);
        for (alpm_list_t *pkgs = alpm_db_get_pkgcache(db); pkgs; pkgs = alpm_list_next(pkgs)) {
            alpm_pkg_t *pkg = (alpm_pkg_t *)alpm_list_getdata(pkgs);
            Package *p = new Package(pkg, localdb);
            Package *parray[] = { p };
            if (!std::includes(packages.begin(), packages.end(),
                               parray, parray + 1,
                               boost::bind(&Filter::cmp, _1, _2, A_NAME))) {
                packages.push_back(p);
            } else
                delete p;
        }
        std::sort(packages.begin(), packages.end(), boost::bind(&Filter::cmp, _1, _2, A_NAME));
    }
    alpm_list_free(dbs);

    if (alpm_release(handle) != 0)
        throw PcursesException("failed to deinitialize alpm library");


    filteredpackages = packages;
}

void Program::init_curses()
{
    (void)system("clear");

    setlocale(LC_ALL, "");

    initscr();
    start_color();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    noecho();

    /* getch() is our loop speed limiter. we use the nonblocking version
      to handle window resizes without waiting for the next key event, but
      use a timeout of 50 ms to limit the cpu usage to acceptable levels.
      50 ms is just an initial value and is subject to change depending on how
      well it works */
    timeout(50);

    /* target ist archlinux so we know a proper ncurses will be used.
       otherwise we would need to conditionally include this using
       NCURSES_VERSION */
    use_default_colors();

    ensureminwsize(COLS, LINES);

    init_pair(5, -1, -1);                   /* default (pane BG) */
    init_pair(2, COLOR_GREEN, -1);          /* default highlight 1 */
    init_pair(3, COLOR_CYAN, -1);           /* default highlight 2 */
    init_pair(6, COLOR_BLUE, -1);
    init_pair(7, COLOR_MAGENTA, -1);
    init_pair(8, COLOR_RED, -1);
    init_pair(9, COLOR_YELLOW, -1);
    init_pair(1, COLOR_BLACK, COLOR_WHITE); /* inverted (status bar BG) */
    init_pair(4, COLOR_BLUE, COLOR_WHITE);  /* inverted highlight 1 */

    list_pane = new CursesListBox(new FrameInfo(FE_LIST, COLS, LINES));
    info_pane = new CursesFrame(new FrameInfo(FE_INFO, COLS, LINES));
    queue_pane = new CursesListBox(new FrameInfo(FE_QUEUE, COLS, LINES));
    status_pane = new CursesFrame(new FrameInfo(FE_STATUS, COLS, LINES));
    input_pane = new CursesFrame(new FrameInfo(FE_INPUT, COLS, LINES));
    help_pane = new CursesFrame(new FrameInfo(FE_HELP, COLS, LINES));

    list_pane->setbackground(C_DEF);
    info_pane->setbackground(C_DEF);
    queue_pane->setbackground(C_DEF);
    status_pane->setbackground(C_INV);
    input_pane->setbackground(C_DEF);
    help_pane->setbackground(C_DEF);

    setfocus(list_pane);
    list_pane->setlist(&filteredpackages);
    queue_pane->setlist(&opqueue);
}

void Program::deinit_curses()
{
    delete list_pane;
    delete queue_pane;
    delete info_pane;
    delete status_pane;
    delete input_pane;
    delete help_pane;

    nocbreak();
    curs_set(1);
    echo();

    endwin();

    (void)system("clear");
}

void Program::printinfosection(AttributeEnum attr, string text)
{
    string caption = AttributeInfo::attrname(attr);
    char hllower = AttributeInfo::attrtochar(attr);
    char hlupper = toupper(hllower);
    bool hldone = false;
    int style;

    for (uint i = 0; i < caption.size(); i++) {
        if (!hldone && (caption[i] == hllower || caption[i] == hlupper)) {
            style = C_DEF;
            hldone = true;
        } else style = C_DEF_HL2;


        info_pane->printw(string(1, caption[i]), style);
    }
    info_pane->printw(": ", C_DEF_HL2);

    string txt = text + "\n";
    info_pane->printw(txt);
}

void Program::updatedisplay()
{
    /* this runs **at least** once per loop iteration
       for example it can run more than once if we need to display
       a 'processing' message during filtering
     */

    if (mode == MODE_INPUT || mode == MODE_STANDARD) {
        Package *pkg;

        erase();
        list_pane->clear();
        info_pane->clear();
        status_pane->clear();
        input_pane->clear();
        queue_pane->clear();

        /* info pane */
        pkg = focused_pane->focusedpackage();
        if (pkg) {
            for (int i = 0; i < A_NONE; i++) {
                AttributeEnum attr = (AttributeEnum)i;
                string txt = pkg->getattr(attr);
                if (txt.length() != 0)
                    printinfosection(attr, txt);
            }
        }

        /* status bar */
        status_pane->mvprintw(1, 0, "Sorted by: ", C_INV_HL1);
        status_pane->printw(AttributeInfo::attrname(sortedby), C_INV);
        status_pane->printw(" Colored by: ", C_INV_HL1);
        status_pane->printw(AttributeInfo::attrname(coloredby), C_INV);
        status_pane->printw(" Filtered by: ", C_INV_HL1);
        status_pane->printw(((searchphrases.length() == 0) ? "-" : searchphrases), C_INV);

        wnoutrefresh(stdscr);
        list_pane->refresh();
        queue_pane->refresh();
        info_pane->refresh();
        status_pane->refresh();

        if (mode == MODE_INPUT) {
            input_pane->printw(optostr(op) + inputbuf.getcontents());
            input_pane->move(inputbuf.getpos() + 1, 0);
            input_pane->refresh();
        }
    } else if (mode == MODE_HELP) {
        help_pane->clear();
        print_help();
        help_pane->refresh();
    }

    doupdate();
}

string Program::optostr(FilterOperationEnum o) const
{
    switch (o) {
    case OP_FILTER: return "/";
    case OP_SORT: return ".";
    case OP_SEARCH: return "?";
    case OP_COLORCODE: return ";";
    case OP_EXEC: return "!";
    case OP_MACRO: return "@";
    case OP_NONE: return "";
    default: assert(0);
    }

    return "";
}

FilterOperationEnum Program::strtoopt(string str) const
{
    for (int i = 0; i < OP_NONE; i++) {
        if (optostr((FilterOperationEnum)i) == str) {
            return (FilterOperationEnum)i;
        }
    }
    return OP_NONE;
}

void Program::clearfilter()
{
    filteredpackages = packages;
    std::sort(filteredpackages.begin(), filteredpackages.end(),
              boost::bind(&Filter::cmp, _1, _2, sortedby));

    searchphrases = "";
    list_pane->moveabs(0);
}

History *Program::gethis(FilterOperationEnum o)
{
    History *v = NULL;

    switch (o) {
    case OP_FILTER: v = &hisfilter; break;
    case OP_SORT: v = &hissort; break;
    case OP_SEARCH: v = &hissearch; break;
    case OP_COLORCODE: v = &hiscolorcode; break;
    case OP_EXEC: v = &hisexec; break;
    case OP_MACRO: v = &hismacro; break;
    default: assert(0);
    }

    return v;
}

void Program::execmacro(string str)
{
    gethis(OP_MACRO)->add(str);

    /* macro delimiter is ',' */
    vector<string> strs;
    boost::split(strs, str, boost::is_any_of(","));

    for (uint i = 0; i < strs.size(); i++) {
        string macropart = strs[i];
        boost::trim(macropart);

        map<string, string>::iterator it;
        it = macros->find(macropart);

        if (it == macros->end()) {
            return;
        }

        string cmd = it->second;

        FilterOperationEnum op = strtoopt(cmd.substr(0, 1));
        if (op == OP_NONE) {
            return;
        }

        inputbuf.set(cmd.substr(1));
        exitinputmode(op);
    }
}

void Program::execmd(string str)
{
    gethis(OP_EXEC)->add(str);

    string pkgs = "";
    for (uint i = 0; i < opqueue.size(); i++) {
        pkgs += opqueue[i]->getname() + " ";
    }

    const string needle = "%p";
    size_t  pos;
    while ((pos = str.find(needle)) != string::npos) {
        str.replace(pos, needle.length(), pkgs);
    }

    deinit_curses();
    run_cmd(str);
    init_curses();
}

void Program::colorcodepackages(string str)
{
    if (str.length() < 1)
        return;

    gethis(OP_COLORCODE)->add(str);

    AttributeEnum attr = A_NONE;
    uint i = 0;

    while (attr == A_NONE && i < str.length()) {
        attr = AttributeInfo::chartoattr(str[i]);
        i++;
    }

    if (attr == A_NONE)
        return;

    Filter::clearattrs();

    vector<Package *>::iterator it = packages.begin();
    for (; it != packages.end(); it++)
        Filter::assigncol(*it, attr);

    coloredby = attr;
}

void Program::searchpackages(string str)
{
    string fieldlist, searchphrase;

    gethis(OP_SEARCH)->add(str);

    /* first, split actual search phrase from field prefix */
    sregex reprefix = sregex::compile("^([A-Za-z]*):(.*)");
    smatch what;

    Filter::clearattrs();
    if (regex_search(str, what, reprefix)) {
        fieldlist = what[1];
        searchphrase = what[2];
        Filter::setattrs(fieldlist);
    } else {
        searchphrase = str;
    }

    /* if search phrase is empty, nothing to do */
    if (searchphrase.length() == 0) {
        return;
    }

    /* we start the search at the current package */
    vector<Package *>::iterator begin = filteredpackages.begin() + list_pane->focusedindex() + 1;
    vector<Package *>::iterator it;

    it = std::find_if(begin, filteredpackages.end(),
                      boost::bind(&Filter::matches, _1, searchphrase));

    /* if not found (and original search didn't start at beginning) wrap around */
    if (it == filteredpackages.end() && begin != filteredpackages.begin()) {
        it = std::find_if(filteredpackages.begin(), filteredpackages.end(),
                          boost::bind(&Filter::matches, _1, searchphrase));
    }

    /* not found, do nothing */
    if (it == filteredpackages.end())
        return;

    /* move focus to found pkg */
    list_pane->moveabs(it - filteredpackages.begin());
}

void Program::sortpackages(string str)
{
    if (str.length() < 1)
        return;

    gethis(OP_SORT)->add(str);

    AttributeEnum attr = A_NONE;
    uint i = 0;

    while (attr == A_NONE && i < str.length()) {
        attr = AttributeInfo::chartoattr(str[i]);
        i++;
    }

    if (attr == A_NONE)
        return;

    sortedby = attr;

    std::sort(filteredpackages.begin(), filteredpackages.end(),
              boost::bind(&Filter::cmp, _1, _2, attr));
}

void Program::filterpackages(string str)
{
    string fieldlist, searchphrase, negate;

    gethis(OP_FILTER)->add(str);

    /* first, split actual search phrase from field prefix */
    sregex reprefix = sregex::compile("^(([A-Za-zq]*)([!]?):)?(.*)");
    smatch what;

    Filter::clearattrs();
    if (regex_search(str, what, reprefix)) {
        fieldlist = what[2];
        negate = what[3];
        searchphrase = what[4];
    } else {
        throw PcursesException("Could not match filter regex.");
    }

    /* if search phrase is empty, nothing to do */
    if (searchphrase.length() == 0) {
        return;
    }

    if (!fieldlist.empty()) {
        Filter::setattrs(fieldlist);
    }

    /* if search phrase is alphanumeric only,
       perform a fast and simple search, else run slower regexp search */

    sregex resimple = sregex::compile("[:alnum:]+");

    /* catch invalid regex input by user */
    try {
        if (regex_match(searchphrase, what, resimple)) {
            bool (*fn)(const Package *, const string) = negate.empty()
                    ? &Filter::notmatches : &Filter::matches;
            vector<Package *>::iterator it =
                std::find_if(filteredpackages.begin(), filteredpackages.end(),
                             boost::bind(fn, _1, searchphrase));
            while (it != filteredpackages.end()) {
                filteredpackages.erase(it);
                it = std::find_if(it, filteredpackages.end(),
                                  boost::bind(fn, _1, searchphrase));
            }
        } else {
            bool (*fn)(const Package *, const sregex) = negate.empty()
                    ? &Filter::notmatchesre : &Filter::matchesre;
            sregex needle = sregex::compile(searchphrase, icase);
            vector<Package *>::iterator it =
                std::find_if(filteredpackages.begin(), filteredpackages.end(),
                             boost::bind(fn, _1, needle));
            while (it != filteredpackages.end()) {
                filteredpackages.erase(it);
                it = std::find_if(it, filteredpackages.end(),
                                  boost::bind(fn, _1, needle));
            }
        }

        if (searchphrases.length() != 0) searchphrases += ", ";
        searchphrases += str;

    } catch (boost::xpressive::regex_error &e) {
        /* we don't have any decent feedback mechanisms, so ignore faulty regexp */
    }
}
