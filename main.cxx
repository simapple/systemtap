// systemtap translator/driver
// Copyright (C) 2005-2011 Red Hat Inc.
// Copyright (C) 2005 IBM Corp.
// Copyright (C) 2006 Intel Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "parse.h"
#include "elaborate.h"
#include "translate.h"
#include "buildrun.h"
#include "session.h"
#include "hash.h"
#include "cache.h"
#include "util.h"
#include "coveragedb.h"
#include "rpm_finder.h"
#include "task_finder.h"
#include "csclient.h"
#include "remote.h"

#include <libintl.h>
#include <locale.h>

#include "stap-probe.h"

#include <cstdlib>

extern "C" {
#include <glob.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
}

using namespace std;

static void
uniq_list(list<string>& l)
{
  set<string> s;
  list<string>::iterator i = l.begin();
  while (i != l.end())
    if (s.insert(*i).second)
      ++i;
    else
      i = l.erase(i);
}

static void
printscript(systemtap_session& s, ostream& o)
{
  if (s.listing_mode)
    {
      // We go through some heroic measures to produce clean output.
      // Record the alias and probe pointer as <name, set<derived_probe *> >
      map<string,set<derived_probe *> > probe_list;

      // Pre-process the probe alias
      for (unsigned i=0; i<s.probes.size(); i++)
        {
          if (pending_interrupts) return;

          derived_probe* p = s.probes[i];
          // NB: p->basest() is not so interesting;
          // p->almost_basest() doesn't quite work, so ...
          vector<probe*> chain;
          p->collect_derivation_chain (chain);
          probe* second = (chain.size()>1) ? chain[chain.size()-2] : chain[0];

          if (s.verbose > 5) {
          p->printsig(cerr); cerr << endl;
          cerr << "chain[" << chain.size() << "]:" << endl;
          for (unsigned j=0; j<chain.size(); j++)
            {
              cerr << "  [" << j << "]: " << endl;
              cerr << "\tlocations[" << chain[j]->locations.size() << "]:" << endl;
              for (unsigned k=0; k<chain[j]->locations.size(); k++)
                {
                  cerr << "\t  [" << k << "]: ";
                  chain[j]->locations[k]->print(cerr);
                  cerr << endl;
                }
              const probe_alias *a = chain[j]->get_alias();
              if (a)
                {
                  cerr << "\taliases[" << a->alias_names.size() << "]:" << endl;
                  for (unsigned k=0; k<a->alias_names.size(); k++)
                    {
                      cerr << "\t  [" << k << "]: ";
                      a->alias_names[k]->print(cerr);
                      cerr << endl;
                    }
                }
            }
          }

          stringstream tmps;
          const probe_alias *a = second->get_alias();
          if (a)
            {
              assert (a->alias_names.size() >= 1);
              a->alias_names[0]->print(tmps); // XXX: [0] is arbitrary; perhaps print all
            }
          else
            {
              assert (second->locations.size() >= 1);
              second->locations[0]->print(tmps); // XXX: [0] is less arbitrary here, but still ...
            }
          string pp = tmps.str();

          // Now duplicate-eliminate.  An alias may have expanded to
          // several actual derived probe points, but we only want to
          // print the alias head name once.
          probe_list[pp].insert(p);
        }

      // print probe name and variables if there
      for (map<string, set<derived_probe *> >::iterator it=probe_list.begin(); it!=probe_list.end(); ++it)
        {
          o << it->first; // probe name or alias

          // Print the locals and arguments for -L mode only
          if (s.listing_mode_vars)
            {
              map<string,unsigned> var_count; // format <"name:type",count>
              map<string,unsigned> arg_count;
              list<string> var_list;
              list<string> arg_list;
              // traverse set<derived_probe *> to collect all locals and arguments
              for (set<derived_probe *>::iterator ix=it->second.begin(); ix!=it->second.end(); ++ix)
                {
                  derived_probe* p = *ix;
                  // collect available locals of the probe
                  for (unsigned j=0; j<p->locals.size(); j++)
                    {
                      stringstream tmps;
                      vardecl* v = p->locals[j];
                      v->printsig (tmps);
                      var_count[tmps.str()]++;
		      var_list.push_back(tmps.str());
                    }
                  // collect arguments of the probe if there
                  list<string> arg_set;
                  p->getargs(arg_set);
                  for (list<string>::iterator ia=arg_set.begin(); ia!=arg_set.end(); ++ia) {
                    arg_count[*ia]++;
                    arg_list.push_back(*ia);
		  }
                }

	      uniq_list(arg_list);
	      uniq_list(var_list);

              // print the set-intersection only
              for (list<string>::iterator ir=var_list.begin(); ir!=var_list.end(); ++ir)
                if (var_count.find(*ir)->second == it->second.size()) // print locals
                  o << " " << *ir;
              for (list<string>::iterator ir=arg_list.begin(); ir!=arg_list.end(); ++ir)
                if (arg_count.find(*ir)->second == it->second.size()) // print arguments
                  o << " " << *ir;
            }
          o << endl;
        }
    }
  else
    {
      if (s.embeds.size() > 0)
        o << _("# global embedded code") << endl;
      for (unsigned i=0; i<s.embeds.size(); i++)
        {
          if (pending_interrupts) return;
          embeddedcode* ec = s.embeds[i];
          ec->print (o);
          o << endl;
        }

      if (s.globals.size() > 0)
        o << _("# globals") << endl;
      for (unsigned i=0; i<s.globals.size(); i++)
        {
          if (pending_interrupts) return;
          vardecl* v = s.globals[i];
          v->printsig (o);
          if (s.verbose && v->init)
            {
              o << " = ";
              v->init->print(o);
            }
          o << endl;
        }

      if (s.functions.size() > 0)
        o << _("# functions") << endl;
      for (map<string,functiondecl*>::iterator it = s.functions.begin(); it != s.functions.end(); it++)
        {
          if (pending_interrupts) return;
          functiondecl* f = it->second;
          f->printsig (o);
          o << endl;
          if (f->locals.size() > 0)
            o << _("  # locals") << endl;
          for (unsigned j=0; j<f->locals.size(); j++)
            {
              vardecl* v = f->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (s.verbose)
            {
              f->body->print (o);
              o << endl;
            }
        }

      if (s.probes.size() > 0)
        o << _("# probes") << endl;
      for (unsigned i=0; i<s.probes.size(); i++)
        {
          if (pending_interrupts) return;
          derived_probe* p = s.probes[i];
          p->printsig (o);
          o << endl;
          if (p->locals.size() > 0)
            o << _("  # locals") << endl;
          for (unsigned j=0; j<p->locals.size(); j++)
            {
              vardecl* v = p->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (s.verbose)
            {
              p->body->print (o);
              o << endl;
            }
        }
    }
}


int pending_interrupts;

extern "C"
void handle_interrupt (int sig)
{
  kill_stap_spawn(sig);
  pending_interrupts ++;
  if (pending_interrupts > 1) // XXX: should be configurable? time-based?
    {
      char msg[] = "Too many interrupts received, exiting.\n";
      int rc = write (2, msg, sizeof(msg)-1);
      if (rc) {/* Do nothing; we don't care if our last gasp went out. */ ;}
      _exit (1);
    }
}


void
setup_signals (sighandler_t handler)
{
  struct sigaction sa;

  sa.sa_handler = handler;
  sigemptyset (&sa.sa_mask);
  if (handler != SIG_IGN)
    {
      sigaddset (&sa.sa_mask, SIGHUP);
      sigaddset (&sa.sa_mask, SIGPIPE);
      sigaddset (&sa.sa_mask, SIGINT);
      sigaddset (&sa.sa_mask, SIGTERM);
    }
  sa.sa_flags = SA_RESTART;

  sigaction (SIGHUP, &sa, NULL);
  sigaction (SIGPIPE, &sa, NULL);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
}

int parse_kernel_config (systemtap_session &s)
{
  // PR10702: pull config options
  string kernel_config_file = s.kernel_build_tree + "/.config";
  struct stat st;
  int rc = stat(kernel_config_file.c_str(), &st);
  if (rc != 0)
    {
        clog << _F("Checking \"%s\" failed with error: %s",
                   kernel_config_file.c_str(), strerror(errno)) << endl;
	//clog << "Checking \"" << kernel_config_file << "\" failed: " << strerror(errno) << endl;
	find_devel_rpms(s, s.kernel_build_tree.c_str());
	missing_rpm_list_print(s,"-devel");
	return rc;
    }

  ifstream kcf (kernel_config_file.c_str());
  string line;
  while (getline (kcf, line))
    {
      if (!startswith(line, "CONFIG_")) continue;
      size_t off = line.find('=');
      if (off == string::npos) continue;
      string key = line.substr(0, off);
      string value = line.substr(off+1, string::npos);
      s.kernel_config[key] = value;
    }
  if (s.verbose > 2)
    clog << _F("Parsed kernel \"%s\", ", kernel_config_file.c_str())
         << _NF("containing %zu tuple", "containing %zu tuples",
                s.kernel_config.size(), s.kernel_config.size()) << endl;
    //clog << "Parsed kernel \"" << kernel_config_file << "\", number of tuples: " << s.kernel_config.size() << endl;

  kcf.close();
  return 0;
}


int parse_kernel_exports (systemtap_session &s)
{
  string kernel_exports_file = s.kernel_build_tree + "/Module.symvers";
  struct stat st;
  int rc = stat(kernel_exports_file.c_str(), &st);
  if (rc != 0)
    {
        clog << _F("Checking \"%s\" failed with error: %s",
                   kernel_exports_file.c_str(), strerror(errno)) << endl
	//clog << "Checking \"" << kernel_exports_file << "\" failed: " << strerror(errno) << endl
	     << _("Ensure kernel development headers & makefiles are installed.") << endl;
	return rc;
    }

  ifstream kef (kernel_exports_file.c_str());
  string line;
  while (getline (kef, line))
    {
      vector<string> tokens;
      tokenize (line, tokens, "\t");
      if (tokens.size() == 4 &&
          tokens[2] == "vmlinux" &&
          tokens[3].substr(0,13) == string("EXPORT_SYMBOL"))
        s.kernel_exports.insert (tokens[1]);
    }
  if (s.verbose > 2)
    clog << _F("Parsed kernel \"%s\", ", kernel_exports_file.c_str())
         << _NF("which contained one vmlinux export", "which contained %zu vmlinux exports",
                s.kernel_exports.size(), s.kernel_exports.size()) << endl;
    //clog << "Parsed kernel \"" << kernel_exports_file << "\", number of vmlinux exports: " << s.kernel_exports.size() << endl;

  kef.close();
  return 0;
}


static int
create_temp_dir (systemtap_session &s)
{
  if (!s.tmpdir.empty())
    return 0;

  // Create a temporary directory to build within.
  // Be careful with this, as "tmpdir" is "rm -rf"'d at the end.
  const char* tmpdir_env = getenv("TMPDIR");
  if (! tmpdir_env)
    tmpdir_env = "/tmp";

  string stapdir = "/stapXXXXXX";
  string tmpdirt = tmpdir_env + stapdir;
  mode_t mask = umask(0);
  const char *tmpdir_name = mkdtemp((char *)tmpdirt.c_str());
  umask(mask);
  if (! tmpdir_name)
    {
      const char* e = strerror (errno);
      //we can't make the directory due to the error
      cerr << _F("ERROR: cannot create temporary directory (\" %s \"): %s", tmpdirt.c_str(), e) << endl;
      //cerr << "ERROR: cannot create temporary directory (\"" << tmpdirt << "\"): " << e << endl;
      return 1;
    }
  else
    s.tmpdir = tmpdir_name;

  if (s.verbose>1)
    clog << _F("Created temporary directory \"%s\"", s.tmpdir.c_str()) << endl;
    //clog << "Created temporary directory \"" << s.tmpdir << "\"" << endl;
  return 0;
}

static void
remove_temp_dir (systemtap_session &s)
{
  if (!s.tmpdir.empty())
    {
      if (s.keep_tmpdir)
        // NB: the format of this message needs to match the expectations
        // of stap-server-connect.c.
        clog << _F("Keeping temporary directory \"%s\"", s.tmpdir.c_str()) << endl;
        //clog << "Keeping temporary directory \"" << s.tmpdir << "\"" << endl;
      else
        {
	  // Ignore signals while we're deleting the temporary directory.
	  setup_signals (SIG_IGN);

	  // Remove the temporary directory.
	  vector<string> cleanupcmd;
	  cleanupcmd.push_back("rm");
	  cleanupcmd.push_back("-rf");
	  cleanupcmd.push_back(s.tmpdir);

	  (void) stap_system (s.verbose, cleanupcmd);
          s.tmpdir.clear();
        }
    }
}

// Compilation passes 0 through 4
static int
passes_0_4 (systemtap_session &s)
{
  int rc = 0;

  // If we don't know the release, there's no hope either locally or on a server.
  if (s.kernel_release.empty())
    {
      if (s.kernel_build_tree.empty())
        cerr << _("ERROR: kernel release isn't specified") << endl;
      else
        cerr << _F("ERROR: kernel release isn't found in \"%s\"",
                   s.kernel_build_tree.c_str()) << endl;
      return 1;
    }

  // Create a temporary directory to build within.
  // Be careful with this, as "s.tmpdir" is "rm -rf"'d at the end.
  rc = create_temp_dir (s);
  if (rc)
    return rc;

  // Perform passes 0 through 4 using a compile server?
  if (! s.specified_servers.empty ())
    {
#if HAVE_NSS
      compile_server_client client (s);
      return client.passes_0_4 ();
#else
      cerr << _("WARNING: Without NSS, using a compile-server is not supported by this version of systemtap") << endl;
      // This cannot be an attempt to use a server after a local compile failed
      // since --use-server-on-error is locked to 'no' if we don't have
      // NSS.
      assert (! s.try_server ());
#endif
    }

  // PASS 0: setting up
  s.verbose = s.perpass_verbose[0];
  PROBE1(stap, pass0__start, &s);

  // For PR1477, we used to override $PATH and $LC_ALL and other stuff
  // here.  We seem to use complete pathnames in
  // buildrun.cxx/tapsets.cxx now, so this is not necessary.  Further,
  // it interferes with util.cxx:find_executable(), used for $PATH
  // resolution.

  s.kernel_base_release.assign(s.kernel_release, 0, s.kernel_release.find('-'));

  // arguments parsed; get down to business
  if (s.verbose > 1)
    {
      s.version ();
      clog << _F("Session arch: %s release: %s", s.architecture.c_str(), s.kernel_release.c_str()) << endl;
      //clog << "Session arch: " << s.architecture
           //<< " release: " << s.kernel_release
           //<< endl;
    }

  // Now that no further changes to s.kernel_build_tree can occur, let's use it.
  if ((rc = parse_kernel_config (s)) != 0)
    {
      // Try again with a server
      s.set_try_server ();
      return rc;
    }

  if ((rc = parse_kernel_exports (s)) != 0)
    {
      // Try again with a server
      s.set_try_server ();
      return rc;
    }

  // Create the name of the C source file within the temporary
  // directory.
  s.translated_source = string(s.tmpdir) + "/" + s.module_name + ".c";

  // Set up our handler to catch routine signals, to allow clean
  // and reasonably timely exit.
  setup_signals(&handle_interrupt);

  PROBE1(stap, pass0__end, &s);

  struct tms tms_before;
  times (& tms_before);
  struct timeval tv_before;
  gettimeofday (&tv_before, NULL);

  // PASS 1a: PARSING USER SCRIPT
  PROBE1(stap, pass1a__start, &s);

  struct stat user_file_stat;
  int user_file_stat_rc = -1;

  if (s.script_file == "-")
    {
      s.user_file = parse (s, cin, s.guru_mode);
      user_file_stat_rc = fstat (STDIN_FILENO, & user_file_stat);
    }
  else if (s.script_file != "")
    {
      s.user_file = parse (s, s.script_file, s.guru_mode);
      user_file_stat_rc = stat (s.script_file.c_str(), & user_file_stat);
    }
  else
    {
      istringstream ii (s.cmdline_script);
      s.user_file = parse (s, ii, s.guru_mode);
    }
  if (s.user_file == 0)
    {
      // Syntax errors already printed.
      rc ++;
    }

  // Construct arch / kernel-versioning search path
  vector<string> version_suffixes;
  string kvr = s.kernel_release;
  const string& arch = s.architecture;
  // add full kernel-version-release (2.6.NN-FOOBAR) + arch
  version_suffixes.push_back ("/" + kvr + "/" + arch);
  version_suffixes.push_back ("/" + kvr);
  // add kernel version (2.6.NN) + arch
  if (kvr != s.kernel_base_release) {
    kvr = s.kernel_base_release;
    version_suffixes.push_back ("/" + kvr + "/" + arch);
    version_suffixes.push_back ("/" + kvr);
  }
  // add kernel family (2.6) + arch
  string::size_type dot1_index = kvr.find ('.');
  string::size_type dot2_index = kvr.rfind ('.');
  while (dot2_index > dot1_index && dot2_index != string::npos) {
    kvr.erase(dot2_index);
    version_suffixes.push_back ("/" + kvr + "/" + arch);
    version_suffixes.push_back ("/" + kvr);
    dot2_index = kvr.rfind ('.');
  }
  // add architecture search path
  version_suffixes.push_back("/" + arch);
  // add empty string as last element
  version_suffixes.push_back ("");

  // PASS 1b: PARSING LIBRARY SCRIPTS
  PROBE1(stap, pass1b__start, &s);

  set<pair<dev_t, ino_t> > seen_library_files;

  for (unsigned i=0; i<s.include_path.size(); i++)
    {
      // now iterate upon it
      for (unsigned k=0; k<version_suffixes.size(); k++)
        {
          glob_t globbuf;
          string dir = s.include_path[i] + version_suffixes[k] + "/*.stp";
          int r = glob(dir.c_str (), 0, NULL, & globbuf);
          if (r == GLOB_NOSPACE || r == GLOB_ABORTED)
	    rc ++;
	  // GLOB_NOMATCH is acceptable

          unsigned prev_s_library_files = s.library_files.size();

          for (unsigned j=0; j<globbuf.gl_pathc; j++)
            {
              if (pending_interrupts)
                break;

              struct stat tapset_file_stat;
              int stat_rc = stat (globbuf.gl_pathv[j], & tapset_file_stat);
              if (stat_rc == 0 && user_file_stat_rc == 0 &&
                  user_file_stat.st_dev == tapset_file_stat.st_dev &&
                  user_file_stat.st_ino == tapset_file_stat.st_ino)
                {
                  cerr 
                  << _F("usage error: tapset file '%s' cannot be run directly as a session script.",
                        globbuf.gl_pathv[j]) << endl;
                  //cerr << "usage error: tapset file '" << globbuf.gl_pathv[j]
                  //     << "' cannot be run directly as a session script." << endl;
                  rc ++;
                }

              // PR11949: duplicate-eliminate tapset files
              if (stat_rc == 0)
                {
                  pair<dev_t,ino_t> here = make_pair(tapset_file_stat.st_dev,
                                                     tapset_file_stat.st_ino);
                  if (seen_library_files.find(here) != seen_library_files.end())
                    continue;
                  seen_library_files.insert (here);
                }

              // XXX: privilege only for /usr/share/systemtap?
              stapfile* f = parse (s, globbuf.gl_pathv[j], true);
              if (f == 0 && !s.suppress_warnings)
                s.print_warning("tapset '" + string(globbuf.gl_pathv[j])
                                + "' has errors, and will be skipped.");
              else
                s.library_files.push_back (f);
            }

          unsigned next_s_library_files = s.library_files.size();
          if (s.verbose>1 && globbuf.gl_pathc > 0)
            // search a directory, report a number of found  with number of processed 
            clog << _F("Searched: \" %s \", found: %zu, processed: %u",
                       dir.c_str(), globbuf.gl_pathc,
                       (next_s_library_files-prev_s_library_files)) << endl;
            //clog << "Searched \"" << dir << "\","
            //     << " found " << globbuf.gl_pathc 
            //     << " processed " << (next_s_library_files-prev_s_library_files) << endl;

          globfree (& globbuf);
        }
    }
  if (s.num_errors())
    rc ++;

  if (rc == 0 && s.last_pass == 1)
    {
      cout << _("# parse tree dump") << endl;
      s.user_file->print (cout);
      cout << endl;
      if (s.verbose)
        for (unsigned i=0; i<s.library_files.size(); i++)
          {
            s.library_files[i]->print (cout);
            cout << endl;
          }
    }

  struct tms tms_after;
  times (& tms_after);
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  struct timeval tv_after;
  gettimeofday (&tv_after, NULL);

#define TIMESPRINT "in " << \
           (tms_after.tms_cutime + tms_after.tms_utime \
            - tms_before.tms_cutime - tms_before.tms_utime) * 1000 / (_sc_clk_tck) << "usr/" \
        << (tms_after.tms_cstime + tms_after.tms_stime \
            - tms_before.tms_cstime - tms_before.tms_stime) * 1000 / (_sc_clk_tck) << "sys/" \
        << ((tv_after.tv_sec - tv_before.tv_sec) * 1000 + \
            ((long)tv_after.tv_usec - (long)tv_before.tv_usec) / 1000) << "real ms."

  // syntax errors, if any, are already printed
  if (s.verbose)
    {
      clog << "Pass 1: parsed user script and "
           << s.library_files.size()
           << " library script(s) "
           << getmemusage()
           << TIMESPRINT
           << endl;
    }

  if (rc && !s.listing_mode)
    cerr << _("Pass 1: parse failed.  Try again with another '--vp 1' option.") << endl;
    //cerr << "Pass 1: parse failed.  "
    //     << "Try again with another '--vp 1' option."
    //     << endl;

  PROBE1(stap, pass1__end, &s);

  if (rc || s.last_pass == 1 || pending_interrupts) return rc;

  times (& tms_before);
  gettimeofday (&tv_before, NULL);

  // PASS 2: ELABORATION
  s.verbose = s.perpass_verbose[1];
  PROBE1(stap, pass2__start, &s);
  rc = semantic_pass (s);

  if (s.listing_mode || (rc == 0 && s.last_pass == 2))
    printscript(s, cout);

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) clog << "Pass 2: analyzed script: "
                      << s.probes.size() << " probe(s), "
                      << s.functions.size() << " function(s), "
                      << s.embeds.size() << " embed(s), "
                      << s.globals.size() << " global(s) "
                      << getmemusage()
                      << TIMESPRINT
                      << endl;

  if (rc && !s.listing_mode && !s.try_server ())
    cerr << _("Pass 2: analysis failed.  Try again with another '--vp 01' option.") << endl;
    //cerr << "Pass 2: analysis failed.  "
    //     << "Try again with another '--vp 01' option."
    //     << endl;

  /* Print out list of missing files.  XXX should be "if (rc)" ? */
  missing_rpm_list_print(s,"-debuginfo");

  PROBE1(stap, pass2__end, &s);

  if (rc || s.listing_mode || s.last_pass == 2 || pending_interrupts) return rc;

  rc = prepare_translate_pass (s);
  if (rc || pending_interrupts) return rc;

  // Generate hash.  There isn't any point in generating the hash
  // if last_pass is 2, since we'll quit before using it.
  if (s.use_script_cache)
    {
      ostringstream o;
      unsigned saved_verbose;

      {
        // Make sure we're in verbose mode, so that printscript()
        // will output function/probe bodies.
        saved_verbose = s.verbose;
        s.verbose = 3;
        printscript(s, o);  // Print script to 'o'
        s.verbose = saved_verbose;
      }

      // Generate hash
      find_script_hash (s, o.str());

      // See if we can use cached source/module.
      if (get_script_from_cache(s))
        {
	  // If our last pass isn't 5, we're done (since passes 3 and
	  // 4 just generate what we just pulled out of the cache).
	  if (s.last_pass < 5 || pending_interrupts) return rc;

	  // Short-circuit to pass 5.
	  return 0;
	}
    }

  // PASS 3: TRANSLATION
  s.verbose = s.perpass_verbose[2];
  times (& tms_before);
  gettimeofday (&tv_before, NULL);
  PROBE1(stap, pass3__start, &s);

  rc = translate_pass (s);
  if (! rc && s.last_pass == 3)
    {
      ifstream i (s.translated_source.c_str());
      cout << i.rdbuf();
    }

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) 
    clog << "Pass 3: translated to C into \""
         << s.translated_source
         << "\" "
         << getmemusage()
         << TIMESPRINT
         << endl;

  if (rc && ! s.try_server ())
    cerr << _("Pass 3: translation failed.  Try again with another '--vp 001' option.") << endl;
    //cerr << "Pass 3: translation failed.  "
    //     << "Try again with another '--vp 001' option."
    //     << endl;

  PROBE1(stap, pass3__end, &s);

  if (rc || s.last_pass == 3 || pending_interrupts) return rc;

  // PASS 4: COMPILATION
  s.verbose = s.perpass_verbose[3];
  times (& tms_before);
  gettimeofday (&tv_before, NULL);
  PROBE1(stap, pass4__start, &s);

  if (s.use_cache)
    {
      find_stapconf_hash(s);
      get_stapconf_from_cache(s);
    }
  rc = compile_pass (s);
  if (! rc && s.last_pass == 4)
    {
      cout << ((s.hash_path == "") ? (s.module_name + string(".ko")) : s.hash_path);
      cout << endl;
    }

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) clog << "Pass 4: compiled C into \""
                      << s.module_name << ".ko"
                      << "\" "
                      << TIMESPRINT
                      << endl;

  if (rc && ! s.try_server ())
    cerr << _("Pass 4: compilation failed.  Try again with another '--vp 0001' option.") << endl;
    //cerr << "Pass 4: compilation failed.  "
    //     << "Try again with another '--vp 0001' option."
    //     << endl;
  else
    {
      // Update cache. Cache cleaning is kicked off at the beginning of this function.
      if (s.use_script_cache)
        add_script_to_cache(s);
      if (s.use_cache)
        add_stapconf_to_cache(s);

      // We may need to save the module in $CWD if the cache was
      // inaccessible for some reason.
      if (! s.use_script_cache && s.last_pass == 4)
        s.save_module = true;

      // Copy module to the current directory.
      if (s.save_module && !pending_interrupts)
        {
	  string module_src_path = s.tmpdir + "/" + s.module_name + ".ko";
	  string module_dest_path = s.module_name + ".ko";
	  copy_file(module_src_path, module_dest_path, s.verbose > 1);
	}
    }

  PROBE1(stap, pass4__end, &s);

  return rc;
}

static int
pass_5 (systemtap_session &s, vector<remote*> targets)
{
  // PASS 5: RUN
  s.verbose = s.perpass_verbose[4];
  struct tms tms_before;
  times (& tms_before);
  struct timeval tv_before;
  gettimeofday (&tv_before, NULL);
  // NB: this message is a judgement call.  The other passes don't emit
  // a "hello, I'm starting" message, but then the others aren't interactive
  // and don't take an indefinite amount of time.
  PROBE1(stap, pass5__start, &s);
  if (s.verbose) clog << _("Pass 5: starting run.") << endl;
  int rc = remote::run(targets);
  struct tms tms_after;
  times (& tms_after);
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  struct timeval tv_after;
  gettimeofday (&tv_after, NULL);
  if (s.verbose) clog << "Pass 5: run completed "
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << _("Pass 5: run failed.  Try again with another '--vp 00001' option.") << endl;
    //cerr << "Pass 5: run failed.  "
    //     << "Try again with another '--vp 00001' option."
    //     << endl;
  else
    // Interrupting pass-5 to quit is normal, so we want an EXIT_SUCCESS below.
    pending_interrupts = 0;

  PROBE1(stap, pass5__end, &s);

  return rc;
}

static void
cleanup (systemtap_session &s, int rc)
{
  // PASS 6: cleaning up
  PROBE1(stap, pass6__start, &s);

  for (systemtap_session::session_map_t::iterator it = s.subsessions.begin();
       it != s.subsessions.end(); ++it)
    cleanup (*it->second, rc);

  // update the database information
  if (!rc && s.tapset_compile_coverage && !pending_interrupts) {
#ifdef HAVE_LIBSQLITE3
    update_coverage_db(s);
#else
    cerr << _("Coverage database not available without libsqlite3") << endl;
#endif
  }

  // Clean up temporary directory.  Obviously, be careful with this.
  remove_temp_dir (s);

  PROBE1(stap, pass6__end, &s);
}

static int
passes_0_4_again_with_server (systemtap_session &s)
{
  // Not a server and not already using a server.
  assert (! s.client_options);
  assert (s.specified_servers.empty ());

  // Specify default server(s).
  s.specified_servers.push_back ("");

  // Remove the previous temporary directory and start fresh.
  remove_temp_dir (s);

  // Try to compile again, using the server
  clog << "Attempting compilation using a compile server"
       << endl;
  int rc = passes_0_4 (s);
  return rc;
}

int
main (int argc, char * const argv [])
{
  // Initialize defaults.
  systemtap_session s;

  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  // Process the command line.
  int rc = s.parse_cmdline (argc, argv);
  if (rc != 0)
    exit (rc);

  // Check for options conflicts. Exits if errors are detected.
  s.check_options (argc, argv);

  // If requested, query server status. This is independent of other tasks.
  query_server_status (s);

  // If requested, manage trust of servers. This is independent of other tasks.
  manage_server_trust (s);

  // Some of the remote methods need to write temporary data, so go ahead
  // and create the main tempdir now.
  rc = create_temp_dir (s);

  // Run the passes only if a script has been specified. The requirement for
  // a script has already been checked in systemtap_session::check_options.
  if (rc == 0 && s.have_script)
    {
      vector<remote*> targets;
      if (s.remote_uris.empty())
	{
	  remote* target = remote::create(s, "direct");
	  if (target)
	    targets.push_back(target);
	  else
	    rc = 1;
	}
      else
	for (unsigned i = 0; i < s.remote_uris.size(); ++i)
	  {
	    remote *target = remote::create(s, s.remote_uris[i]);
	    if (target)
	      targets.push_back(target);
	    else
	      {
		rc = 1;
		break;
	      }
	  }

      // Run passes 0-4 for each unique session,
      // either locally or using a compile-server.
      if (rc == 0)
	{
	  set<systemtap_session*> sessions;
	  for (unsigned i = 0; i < targets.size(); ++i)
	    sessions.insert(targets[i]->get_session());
	  for (set<systemtap_session*>::iterator it = sessions.begin();
	       it != sessions.end(); ++it)
	    {
	      (*it)->init_try_server ();
	      if ((rc = passes_0_4 (**it)))
		{
		  // Compilation failed.
		  // Try again using a server if appropriate.
		  if ((*it)->try_server ())
		    rc = passes_0_4_again_with_server (**it);
		  if (rc)
		    break;
		}
	    }
	}

      // Run pass 5, if requested
      if (rc == 0 && s.last_pass >= 5 && ! pending_interrupts)
	rc = pass_5 (s, targets);

      for (unsigned i = 0; i < targets.size(); ++i)
	delete targets[i];
    }

  // Pass 6. Cleanup
  cleanup (s, rc);

  return (rc||pending_interrupts) ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
